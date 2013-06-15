#include "rpc/ffgate.h"
#include "net/net_factory.h"
#include "base/log.h"
using namespace ff;

#define FFGATE                   "FFGATE"

ffgate_t::ffgate_t()
{
    
}
ffgate_t::~ffgate_t()
{
    
}

int ffgate_t::open(arg_helper_t& arg_helper)
{
    LOGTRACE((FFGATE, "ffgate_t::open begin"));
    if (false == arg_helper.is_enable_option("-gate"))
    {
        LOGERROR((FFGATE, "ffgate_t::open failed without -gate argmuent"));
        return -1;
    }
    m_gate_name = arg_helper.get_option_value("-gate");
    m_ffrpc = new ffrpc_t(m_gate_name);
    
    m_ffrpc->reg(&ffgate_t::change_session_logic, this);
    m_ffrpc->reg(&ffgate_t::close_session, this);
    m_ffrpc->reg(&ffgate_t::route_msg_to_session, this);
    m_ffrpc->reg(&ffgate_t::broadcast_msg_to_session, this);
    
    if (m_ffrpc->open(arg_helper.get_option_value("-broker")))
    {
        LOGERROR((FFGATE, "ffgate_t::open failed check -broker argmuent"));
        return -1;
    }
    
    if (NULL == net_factory_t::gateway_listen(string("-gate_listen ") + arg_helper.get_option_value("-gate_listen"), this))
    {
        LOGERROR((FFGATE, "ffgate_t::open failed without -gate_listen"));
        return -1;
    }
    
    LOGTRACE((FFGATE, "ffgate_t::open end ok"));
    return 0;
}
int ffgate_t::close()
{
    if (m_ffrpc)
    {
        m_ffrpc->close();
    }
    return 0;
}

//! 处理连接断开
int ffgate_t::handle_broken(socket_ptr_t sock_)
{
    m_ffrpc->get_tq().produce(task_binder_t::gen(&ffgate_t::handle_broken_impl, this, sock_));
    return 0;
}
//! 处理消息
int ffgate_t::handle_msg(const message_t& msg_, socket_ptr_t sock_)
{
    m_ffrpc->get_tq().produce(task_binder_t::gen(&ffgate_t::handle_msg_impl, this, msg_, sock_));
    return 0;
}

//! 处理连接断开
int ffgate_t::handle_broken_impl(socket_ptr_t sock_)
{
    session_data_t* session_data = sock_->get_data<session_data_t>();
    if (NULL == session_data)
    {
        sock_->safe_delete();
        return 0;
    }
    
    if (false == session_data->is_valid())
    {
        //! 还未通过验证
        m_wait_verify_set.erase(sock_);
    }
    else
    {
        if (m_client_set[session_data->id()].sock == sock_)
        {
            m_client_set.erase(session_data->id());
            gate_session_offline_t::in_t msg;
            msg.session_id  = session_data->id();
            msg.online_time = session_data->online_time;
            m_ffrpc->call("session_mgr", msg);
        }
    }
    LOGTRACE((FFGATE, "ffgate_t::broken session_id[%s]", session_data->id()));
    delete session_data;
    sock_->set_data(NULL);
    sock_->safe_delete();
    return 0;
}
//! 处理消息
int ffgate_t::handle_msg_impl(const message_t& msg_, socket_ptr_t sock_)
{
    session_data_t* session_data = sock_->get_data<session_data_t>();
    if (NULL == session_data)//! 还未验证sessionid
    {
        return verify_session_id(msg_, sock_);
    }
    else if (false == session_data->is_valid())
    {
        //! sessionid再未验证之前，client是不能发送消息的
        sock_->close();
        return 0;
    }
    else
    {
        return route_logic_msg(msg_, sock_);
    }
    return 0;
}

//! 验证sessionid
int ffgate_t::verify_session_id(const message_t& msg_, socket_ptr_t sock_)
{
    LOGTRACE((FFGATE, "ffgate_t::verify_session_id session_key[%s]", msg_.get_body()));
    session_data_t* session_data = new session_data_t();
    sock_->set_data(session_data);
    //! 还未通过验证
    m_wait_verify_set.insert(sock_);

    gate_session_online_t::in_t msg;
    msg.session_key = msg_.get_body();
    msg.online_time = session_data->online_time;
    msg.gate_name   = m_gate_name;
    m_ffrpc->call("session_mgr", msg, ffrpc_ops_t::gen_callback(&ffgate_t::verify_session_callback, this, sock_));
    LOGTRACE((FFGATE, "ffgate_t::verify_session_id end ok"));
    return 0;
}
//! 验证sessionid 的回调函数
int ffgate_t::verify_session_callback(ffreq_t<gate_session_online_t::out_t>& req_, socket_ptr_t sock_)
{
    LOGTRACE((FFGATE, "ffgate_t::verify_session_callback session_id[%s], err[%s]", req_.arg.session_id, req_.arg.err));
    set<socket_ptr_t>::iterator it = m_wait_verify_set.find(sock_);
    if (it == m_wait_verify_set.end())
    {
        //! 连接已经断开
        return 0;
    }
    m_wait_verify_set.erase(it);
    
    if (false == req_.arg.err.empty())
    {
        sock_->close();
        return 0;
    }
    session_data_t* session_data = sock_->get_data<session_data_t>();
    session_data->set_id(req_.arg.session_id);
    client_info_t& client_info = m_client_set[session_data->id()];
    if (client_info.sock)
    {
        client_info.sock->close();
    }
    client_info.sock = sock_;
    client_info.alloc_logic_service = req_.arg.alloc_logic_service;
    LOGTRACE((FFGATE, "ffgate_t::verify_session_callback alloc_logic_service[%s]", req_.arg.alloc_logic_service));
    return 0;
}

//! 逻辑处理,转发消息到logic service
int ffgate_t::route_logic_msg(const message_t& msg_, socket_ptr_t sock_)
{
    session_data_t* session_data = sock_->get_data<session_data_t>();
    LOGTRACE((FFGATE, "ffgate_t::route_logic_msg session_id[%s]", session_data->id()));
    
    client_info_t& client_info   = m_client_set[session_data->id()];
    if (client_info.request_queue.size() == MAX_MSG_QUEUE_SIZE)
    {
        //!  消息队列超限，关闭sock
        sock_->close();
        return 0;
    }
    
    if (client_info.request_queue.empty())
    {
        gate_route_logic_msg_t::in_t msg;
        msg.session_id = session_data->id();
        msg.body       = msg_.get_body();
        m_ffrpc->call(client_info.alloc_logic_service, msg,
                      ffrpc_ops_t::gen_callback(&ffgate_t::route_logic_msg_callback, this, session_data->id(), sock_));
    }
    else
    {
        client_info.request_queue.push(msg_.get_body());
    }
    LOGTRACE((FFGATE, "ffgate_t::route_logic_msg end ok alloc_logic_service[%s]", client_info.alloc_logic_service));
    return 0;
}

//! 逻辑处理,转发消息到logic service
int ffgate_t::route_logic_msg_callback(ffreq_t<gate_route_logic_msg_t::out_t>& req_, const string& session_id_, socket_ptr_t sock_)
{
    LOGTRACE((FFGATE, "ffgate_t::route_logic_msg_callback session_id[%s]", session_id_));
    map<string/*sessionid*/, client_info_t>::iterator it = m_client_set.find(session_id_);
    if (it == m_client_set.end() || it->second.sock != sock_)
    {
        return 0;
    }
    client_info_t& client_info = it->second;
    if (client_info.request_queue.empty())
    {
        return 0;
    }
    
    gate_route_logic_msg_t::in_t msg;
    msg.session_id = session_id_;
    msg.body       = client_info.request_queue.front();
    m_ffrpc->call(client_info.alloc_logic_service, msg,
                  ffrpc_ops_t::gen_callback(&ffgate_t::route_logic_msg_callback, this, session_id_, sock_));
    
    client_info.request_queue.pop();
    LOGTRACE((FFGATE, "ffgate_t::route_logic_msg_callback end ok queue_size[%d],alloc_logic_service[%s]",
                client_info.request_queue.size(), client_info.alloc_logic_service));
    return 0;
}

//! 改变处理client 逻辑的对应的节点
int ffgate_t::change_session_logic(ffreq_t<gate_change_logic_node_t::in_t, gate_change_logic_node_t::out_t>& req_)
{
    LOGTRACE((FFGATE, "ffgate_t::change_session_logic session_id[%s]", req_.arg.session_id));
    map<string/*sessionid*/, client_info_t>::iterator it = m_client_set.find(req_.arg.session_id);
    if (it == m_client_set.end())
    {
        return 0;
    }
    
    it->second.alloc_logic_service = req_.arg.alloc_logic_service;
    
    gate_change_logic_node_t::out_t out;
    req_.response(out);
    LOGTRACE((FFGATE, "ffgate_t::change_session_logic end ok"));
    return 0;
}

//! 关闭某个session socket
int ffgate_t::close_session(ffreq_t<gate_close_session_t::in_t, gate_close_session_t::out_t>& req_)
{
    LOGTRACE((FFGATE, "ffgate_t::close_session session_id[%s]", req_.arg.session_id));
    
    map<string/*sessionid*/, client_info_t>::iterator it = m_client_set.find(req_.arg.session_id);
    if (it == m_client_set.end())
    {
        return 0;
    }
    it->second.sock->close();
    gate_close_session_t::out_t out;
    req_.response(out);
    LOGTRACE((FFGATE, "ffgate_t::gate_close_session_t end ok"));
    return 0;
}

//! 转发消息给client
int ffgate_t::route_msg_to_session(ffreq_t<gate_route_msg_to_session_t::in_t, gate_route_msg_to_session_t::out_t>& req_)
{
    LOGTRACE((FFGATE, "ffgate_t::route_msg_to_session begin num[%d]", req_.arg.session_id.size()));
    
    for (size_t i = 0; i < req_.arg.session_id.size(); ++i)
    {
        string& session_id = req_.arg.session_id[i];
        LOGTRACE((FFGATE, "ffgate_t::route_msg_to_session session_id[%s]", session_id));

        map<string/*sessionid*/, client_info_t>::iterator it = m_client_set.find(session_id);
        if (it == m_client_set.end())
        {
            continue;
        }

        msg_sender_t::send(it->second.sock, 0, req_.arg.body);
    }
    gate_route_msg_to_session_t::out_t out;
    req_.response(out);
    LOGTRACE((FFGATE, "ffgate_t::route_msg_to_session end ok"));
    return 0;
}

//! 广播消息给所有的client
int ffgate_t::broadcast_msg_to_session(ffreq_t<gate_broadcast_msg_to_session_t::in_t, gate_broadcast_msg_to_session_t::out_t>& req_)
{
    LOGTRACE((FFGATE, "ffgate_t::broadcast_msg_to_session begin"));
    
    map<string/*sessionid*/, client_info_t>::iterator it = m_client_set.begin();
    for (; it != m_client_set.end(); ++it)
    {
        msg_sender_t::send(it->second.sock, 0, req_.arg.body);
    }
    
    gate_broadcast_msg_to_session_t::out_t out;
    req_.response(out);
    LOGTRACE((FFGATE, "ffgate_t::broadcast_msg_to_session end ok"));
    return 0;
}