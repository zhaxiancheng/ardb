/*
 *Copyright (c) 2013-2014, yinqiwen <yinqiwen@gmail.com>
 *All rights reserved.
 *
 *Redistribution and use in source and binary forms, with or without
 *modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Redis nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 *THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 *BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "thread/thread_local.hpp"
#include "db/db.hpp"
#include <sys/types.h>
#include <sys/stat.h>
#include "network.hpp"
#include "repl/repl.hpp"

OP_NAMESPACE_BEGIN
    static ThreadLocal<RedisReplyPool> g_reply_pool;
    static QPSTrack g_total_qps;
    static CountTrack g_total_connections_received;
    static CountTrack g_rejected_connections;

    class ServerLifecycleHandler: public ChannelServiceLifeCycle, public Runnable
    {
            void Run()
            {
                g_db->ScanClients();
            }
            void OnStart(ChannelService* serv, uint32 idx)
            {
                serv->GetTimer().Schedule(this, 1, 1000 / g_db->GetConf().hz, MILLIS);
                g_reply_pool.GetValue().SetMaxSize(g_db->GetConf().reply_pool_size);
            }
            void OnStop(ChannelService* serv, uint32 idx)
            {

            }
            void OnRoutine(ChannelService* serv, uint32 idx)
            {

            }
    };

    class RedisRequestHandler: public ChannelUpstreamHandler<RedisCommandFrame>
    {
        private:
            QPSTrack* qpsTrack;
            ClientContext m_client_ctx;
            Context m_ctx;
            bool m_delete_after_processing;
            RedisReplyPool* pool;

            void MessageReceived(ChannelHandlerContext& ctx, MessageEvent<RedisCommandFrame>& e)
            {
                m_client_ctx.last_interaction_ustime = get_current_epoch_micros();
                m_client_ctx.client = ctx.GetChannel();
                RedisCommandFrame* cmd = e.GetMessage();
                ChannelService& serv = m_client_ctx.client->GetService();
                m_client_ctx.processing = true;
                if (NULL == pool)
                {
                    pool = &(g_reply_pool.GetValue());
                }
                pool->Clear();
                m_ctx.SetReply(&(pool->Allocate()));
                RedisReply& reply = m_ctx.GetReply();
                int ret = g_db->Call(m_ctx, *cmd);
                if (NULL != qpsTrack)
                {
                    qpsTrack->IncMsgCount(1);
                }
                g_total_qps.IncMsgCount(1);
                if (m_delete_after_processing)
                {
                    delete this;
                    return;
                }
                if (reply.type != 0 && !m_ctx.flags.reply_off)
                {
                    m_client_ctx.client->Write(reply);
                    if (m_ctx.flags.reply_skip)
                    {
                        m_ctx.flags.reply_skip = 0;
                        m_ctx.flags.reply_off = 1;
                    }
                }
                if (ret < -1)
                {
                    ChannelService* root = &(m_client_ctx.client->GetService());
                    while (root->GetParent() != NULL)
                    {
                        root = root->GetParent();
                    }
                    root->Stop();
                    return;
                }
                else if (-1 == ret)
                {
                    m_client_ctx.client->Close();
                }
                m_client_ctx.processing = false;
                m_client_ctx.last_interaction_ustime = get_current_epoch_micros();
                m_ctx.ClearState();
                //reply.Clear();
            }
            void ChannelClosed(ChannelHandlerContext& ctx, ChannelStateEvent& e)
            {
                g_db->FreeClient(m_ctx);
            }
            void ChannelConnected(ChannelHandlerContext& ctx, ChannelStateEvent& e)
            {
                g_total_connections_received.Add(1);
                m_client_ctx.uptime = get_current_epoch_micros();
                m_client_ctx.last_interaction_ustime = get_current_epoch_micros();
                m_client_ctx.client = ctx.GetChannel();
                m_client_ctx.clientid.id = ctx.GetChannel()->GetID();
                m_client_ctx.clientid.ctx = &m_ctx;
                //m_client_ctx.client->Attach(&m_ctx, NULL);
                if (!g_db->GetConf().requirepass.empty())
                {
                    m_ctx.authenticated = false;
                }

                //client ip white list
                //ReadLockGuard<SpinRWLock> guard(const_cast<SpinRWLock>(g_db->GetConf().lock));
                if (!g_db->GetConf().trusted_ip.empty())
                {
                    const Address* remote = ctx.GetChannel()->GetRemoteAddress();
                    if (InstanceOf<SocketHostAddress>(remote).OK)
                    {
                        const SocketHostAddress* addr = (const SocketHostAddress*) remote;
                        const std::string& ip = addr->GetHost();
                        if (ip != "127.0.0.1") //allways trust 127.0.0.1
                        {
                            StringTreeSet::const_iterator sit = g_db->GetConf().trusted_ip.begin();
                            while (sit != g_db->GetConf().trusted_ip.end())
                            {
                                if (stringmatchlen(sit->c_str(), sit->size(), ip.c_str(), ip.size(), 0) == 1)
                                {
                                    return;
                                }
                                sit++;
                            }
                            ctx.GetChannel()->Close();
                            g_rejected_connections.Add(1);
                        }
                    }
                }
                else
                {
                    g_db->AddClient(m_ctx);
                }
            }
        public:
            RedisRequestHandler(QPSTrack* track) :
                    qpsTrack(track), m_delete_after_processing(false), pool(NULL)
            {
                m_ctx.client = &m_client_ctx;
                //root_reply.SetPool(&pool);
                //m_ctx.SetReply(&root_reply);
                //pool.SetMaxSize(g_db->GetConf().reply_pool_size);
            }
            bool IsProcessing()
            {
                return m_client_ctx.processing;
            }
            void EnableSelfDeleteAfterProcessing()
            {
                m_delete_after_processing = true;
            }
    };
    static void pipelineInit(ChannelPipeline* pipeline, void* data)
    {
        QPSTrack* init_data = (QPSTrack*) data;
        pipeline->AddLast("decoder", new RedisCommandDecoder);
        pipeline->AddLast("encoder", new RedisReplyEncoder);
        pipeline->AddLast("handler", new RedisRequestHandler(init_data));
    }
    static void pipelineDestroy(ChannelPipeline* pipeline, void* data)
    {
        ChannelHandler* handler = pipeline->Get("decoder");
        DELETE(handler);
        handler = pipeline->Get("encoder");
        DELETE(handler);
        RedisRequestHandler* rhandler = (RedisRequestHandler*) pipeline->Get("handler");
        if (NULL != rhandler && rhandler->IsProcessing())
        {
            rhandler->EnableSelfDeleteAfterProcessing();
        }
        else
        {
            DELETE(rhandler);
        }
    }

    static void init_statistics_setting()
    {
        g_total_qps.name = "total_commands_processed";
        g_total_qps.qpsName = "instantaneous_ops_per_sec";
        Statistics::GetSingleton().AddTrack(&g_total_qps);
        g_total_connections_received.name = "total_connections_received";
        Statistics::GetSingleton().AddTrack(&g_total_connections_received);
        g_rejected_connections.name = "rejected_connections";
        Statistics::GetSingleton().AddTrack(&g_rejected_connections);
    }

    Server::Server() :
            m_service(NULL)
    {

    }
    int Server::Start()
    {
        if (0 != g_repl->Init())
        {
            ERROR_LOG("Failed to init replication service.");
            return -1;
        }
        m_service = new ChannelService(g_db->MaxOpenFiles());
        m_service->SetThreadPoolSize(g_db->GetConf().thread_pool_size);
        ServerLifecycleHandler lifecycle;
        m_service->RegisterLifecycleCallback(&lifecycle);

        ChannelOptions ops;
        ops.tcp_nodelay = true;
        ops.reuse_address = true;
        if (g_db->GetConf().tcp_keepalive > 0)
        {
            ops.keep_alive = g_db->GetConf().tcp_keepalive;
        }

        init_statistics_setting();

        std::vector<QPSTrack> serverQpsTracks;
        serverQpsTracks.resize(g_db->GetConf().servers.size());
        for (uint32 i = 0; i < g_db->GetConf().servers.size(); i++)
        {
            ServerSocketChannel* server = NULL;
            const std::string& host = g_db->GetConf().servers[i].host;
            std::string address = host;
            if (g_db->GetConf().servers[i].port == 0)
            {
                SocketUnixAddress unix_address(host);
                server = m_service->NewServerSocketChannel();
                if (!server->Bind(&unix_address))
                {
                    ERROR_LOG("Failed to bind on %s", host.c_str());
                    goto sexit;
                }
                chmod(host.c_str(), g_db->GetConf().servers[i].unixsocketperm);
            }
            else
            {
                SocketHostAddress socket_address(host, g_db->GetConf().servers[i].port);
                server = m_service->NewServerSocketChannel();
                if (!server->Bind(&socket_address))
                {
                    ERROR_LOG("Failed to bind on %s:%u", host.c_str(), g_db->GetConf().servers[i].port);
                    goto sexit;
                }
                address.append(":").append(stringfromll(g_db->GetConf().servers[i].port));
            }
            server->Configure(ops);
            QPSTrack* serverQPSTrack = NULL;
            if (g_db->GetConf().servers.size() > 1)
            {
                serverQPSTrack = &serverQpsTracks[i];
                serverQPSTrack->name = address + "_total_commands_processed";
                serverQPSTrack->qpsName = address + "_instantaneous_ops_per_sec";
                Statistics::GetSingleton().AddTrack(serverQPSTrack);
            }
            server->SetChannelPipelineInitializor(pipelineInit, serverQPSTrack);
            server->SetChannelPipelineFinalizer(pipelineDestroy, NULL);
            server->BindThreadPool(0, g_db->GetConf().thread_pool_size);
            INFO_LOG("Ardb will accept connections on %s", address.c_str());
        }

        StartCrons();

        INFO_LOG("Ardb started with version %s", ARDB_VERSION);
        m_service->Start();
        StopCrons();
        g_repl->StopService();
        sexit:
        DELETE(m_service);
        return 0;
    }

    Server::~Server()
    {
        StopCrons();
    }
OP_NAMESPACE_END

