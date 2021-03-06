/** $lic$
 * Copyright (C) 2016-2017 by Massachusetts Institute of Technology
 *
 * This file is part of TailBench.
 *
 * If you use this software in your research, we request that you reference the
 * TaiBench paper ("TailBench: A Benchmark Suite and Evaluation Methodology for
 * Latency-Critical Applications", Kasture and Sanchez, IISWC-2016) as the
 * source in any publications that use this software, and that you send us a
 * citation of your work.
 *
 * TailBench is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 */

#include "tbench_server.h"

#include <algorithm>
#include <atomic>
#include <vector>

#include "helpers.h"
#include "server.h"

#include <assert.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

/*******************************************************************************
 * NetworkedServer
 *******************************************************************************/
 std::queue<Request> get_Queue() {
    std::queue<Request> Q;
    return Q;
 }

pthread_t* receive_thread;
pthread_attr_t *attr;
Request *global_req;

pthread_cond_t rec_cv;

NetworkedServer::NetworkedServer(int nthreads, std::string ip, int port, \
        int nclients) 
    : Server(nthreads)
{
    pthread_mutex_init(&sendLock, nullptr);
    pthread_mutex_init(&recvLock, nullptr);
    pthread_cond_init(&rec_cv,nullptr);

    reqbuf = new Request[nthreads]; 

    activeFds.resize(nthreads);

    recvClientHead = 0;

    // Get address info
    int status;
    struct addrinfo hints;
    struct addrinfo* servInfo;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    std::stringstream portstr;
    portstr << port;

    const char* ipstr = (ip.size() > 0) ? ip.c_str() : nullptr;

    if ((status = getaddrinfo(ipstr, portstr.str().c_str(), &hints, &servInfo))\
            != 0) {
        std::cerr << "getaddrinfo() failed: " << gai_strerror(status) \
            << std::endl;
        exit(-1);
    }

    // Create listening socket
    int listener = socket(servInfo->ai_family, servInfo->ai_socktype, \
            servInfo->ai_protocol);
    if (listener == -1) {
        std::cerr << "socket() failed: " << strerror(errno) << std::endl;
        exit(-1);
    }

    int yes = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) \
            == -1)  {
        std::cerr << "setsockopt() failed: " << strerror(errno) << std::endl;
        exit(-1);
    }

    if (bind(listener, servInfo->ai_addr, servInfo->ai_addrlen) == -1) {
        std::cerr << "bind() failed: " << strerror(errno) << std::endl;
        exit(-1);
    }

    if (listen(listener, 10) == -1) {
        std::cerr << "listen() failed: " << strerror(errno) << std::endl;
        exit(-1);
    }

    // Establish connections with clients
    struct sockaddr_storage clientAddr;
    socklen_t clientAddrSize;

    for (int c = 0; c < nclients; ++c) {
        clientAddrSize = sizeof(clientAddr);
        memset(&clientAddr, 0, clientAddrSize);

        int clientFd = accept(listener, \
                reinterpret_cast<struct sockaddr*>(&clientAddr), \
                &clientAddrSize);

        if (clientFd == -1) {
            std::cerr << "accept() failed: " << strerror(errno) << std::endl;
            exit(-1);
        }

        int nodelay = 1;
        if (setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, 
                reinterpret_cast<char*>(&nodelay), sizeof(nodelay)) == -1) {
            std::cerr << "setsockopt(TCP_NODELAY) failed: " << strerror(errno) \
                << std::endl;
            exit(-1);
        }

        clientFds.push_back(clientFd);
    }
}

NetworkedServer::~NetworkedServer() {
    delete reqbuf;
}

void NetworkedServer::removeClient(int fd) {
    auto it = std::find(clientFds.begin(), clientFds.end(), fd);
    clientFds.erase(it);
}

bool NetworkedServer::checkRecv(int recvd, int expected, int fd) {
    bool success = false;
    if (recvd == 0) { // Client exited
        std::cerr << "Client left, removing" << std::endl;
        removeClient(fd);
        success = false;
    } else if (recvd == -1) {
        std::cerr << "recv() failed: " << strerror(errno) \
            << ". Exiting" << std::endl;
        exit(-1);
    } else {
        if (recvd != expected) {
            std::cerr << "ERROR! recvd = " << recvd << ", expected = " \
                << expected << std::endl;
            exit(-1);
        }
        // assert(recvd == expected);
        success = true;
    }

    return success;
}

int NetworkedServer::recvReq_Q() {
    bool success = false;
    Request *req = new Request;
    int fd = -1;
//    std::cerr << "server wants to recv req" << std::endl;
    while(!success && clientFds.size() > 0) {
        int maxFd = -1;
        fd_set readSet;
        FD_ZERO(&readSet);
        for (int f : clientFds) {
            FD_SET(f, &readSet);
            if (f > maxFd) maxFd = f;
        }

        int ret = select(maxFd + 1, &readSet, nullptr, nullptr, nullptr);
        if (ret == -1) {
            std::cerr << "select() failed: " << strerror(errno) << std::endl;
            exit(-1);
        }

        fd = -1;

        for (size_t i = 0; i < clientFds.size(); ++i) {
            size_t idx = (recvClientHead + i) % clientFds.size();
            if (FD_ISSET(clientFds[idx], &readSet)) {
                fd = clientFds[idx];
                break;
            }

        }

        recvClientHead = (recvClientHead + 1) % clientFds.size();

        assert(fd != -1);

        int len = sizeof(Request) - MAX_REQ_BYTES;

        int recvd = recvfull(fd, reinterpret_cast<char*>(req), len, 0);
        success = checkRecv(recvd, len, fd);

        if (!success) continue;

        recvd = recvfull(fd, req->data, req->len, 0);

        success = checkRecv(recvd, req->len, fd);
//	std::cerr << "receive success mark is " << success << std::endl;
        if (!success) continue;

    }
   
    pthread_mutex_lock(&recvLock);
    recvReq_Queue.push(req);
    int Qlen = recvReq_Queue.size();
//    std::cerr << "receive req.." << recvReq_Queue.size() << std::endl;
    fd_Queue.push(fd);
    Qlen_Queue.push(Qlen);
    pthread_cond_signal(&rec_cv);
    pthread_mutex_unlock(&recvLock);
    if (clientFds.size() == 0) {
        return 0;
    }
    else {
        return 1;
    }

}


size_t NetworkedServer::recvReq(int id, void** data) {
  //  std::cerr << "reach here 1 " <<std::endl;
    pthread_mutex_lock(&recvLock);
  //  std::cerr << "reach here 2 " << recvReq_Queue.empty()<<std::endl;
    while(recvReq_Queue.empty())
    {
//	std::cerr << recvReq_Queue.size() << std::endl;
	 pthread_cond_wait(&rec_cv,&recvLock);
    }
 //   std::cerr << "reach here 3 " << std::endl;
    Request *req = recvReq_Queue.front();
    recvReq_Queue.pop();
    
    int fd = fd_Queue.front();
    fd_Queue.pop();
    int QL = Qlen_Queue.front();
    Qlen_Queue.pop();
    uint64_t curNs = getCurNs();
    reqInfo[id].id = req->id;
    reqInfo[id].startNs = curNs;
    reqInfo[id].Qlength = QL;
    activeFds[id] = fd;
    *data = reinterpret_cast<void*>(req->data);
    size_t len = req->len;
    //reqInfo[id].reqlen = len;
    global_req = req;
    pthread_mutex_unlock(&recvLock);
    return len;
    
}

/*size_t NetworkedServer::recvReq(int id, void** data) {
    pthread_mutex_lock(&recvLock);

    bool success = false;
    Request* req;
    int fd = -1;
    
    //std::cerr<<"begin retreive request from client port.."<<std::endl;
    while (!success && clientFds.size() > 0) {
        int maxFd = -1;
        fd_set readSet;
        FD_ZERO(&readSet);
        for (int f : clientFds) {
            FD_SET(f, &readSet);
            if (f > maxFd) maxFd = f;
        }
	
	//std::cerr<<"reach here 1 " <<"maxfd is "<<maxFd<<std::endl;
        int ret = select(maxFd + 1, &readSet, nullptr, nullptr, nullptr);
	//std::cerr<<"reach here 1.5 " <<std::endl;
        if (ret == -1) {
            std::cerr << "select() failed: " << strerror(errno) << std::endl;
            exit(-1);
        }
        
        //std::cerr<<"reach here 2 " << std::endl;
        fd = -1;

        for (size_t i = 0; i < clientFds.size(); ++i) {
            size_t idx = (recvClientHead + i) % clientFds.size();
            if (FD_ISSET(clientFds[idx], &readSet)) {
                fd = clientFds[idx];
                break;
            }
        }
	
	//std::cerr<<"reach here 3" <<std::endl;

        recvClientHead = (recvClientHead + 1) % clientFds.size();

        assert(fd != -1);

        int len = sizeof(Request) - MAX_REQ_BYTES; // Read request header first

        req = &reqbuf[id];
	
	//std::cerr<<"begin recv full...."<<std::endl;
        int recvd = recvfull(fd, reinterpret_cast<char*>(req), len, 0);
       // std::cerr<<"end recv full...."<<std::endl;
        success = checkRecv(recvd, len, fd);
	//std::cerr << "first check "<<success << std::endl;
        if (!success) continue;
        
        recvd = recvfull(fd, req->data, req->len, 0);
        //std::cerr << "second check "<<success << std::endl;
        success = checkRecv(recvd, req->len, fd);
        if (!success) continue;
    }
     //std::cerr<<"finish retreive request from client port..."<<std::endl;

    if (clientFds.size() == 0) {
        std::cerr << "All clients exited. Server finishing" << std::endl;
        exit(0);
    } else {
        uint64_t curNs = getCurNs();
        reqInfo[id].id = req->id;
        reqInfo[id].startNs = curNs;
        activeFds[id] = fd;

        *data = reinterpret_cast<void*>(&req->data);
    }

    pthread_mutex_unlock(&recvLock);

    return req->len;
};*/

void NetworkedServer::sendResp(int id, const void* data, size_t len) {
    pthread_mutex_lock(&sendLock);

    Response* resp = new Response();
    
    resp->type = RESPONSE;
    resp->id = reqInfo[id].id;
    resp->len = len;
    resp->queue_len = reqInfo[id].Qlength;
   // resp->req_len = reqInfo[id].Reqlen;
    memcpy(reinterpret_cast<void*>(&resp->data), data, len);
    uint64_t curNs = getCurNs();
    assert(curNs > reqInfo[id].startNs);
    resp->svcNs = curNs - reqInfo[id].startNs;
    //std::cerr << "send response " << std::endl;
    int fd = activeFds[id];
    int totalLen = sizeof(Response) - MAX_RESP_BYTES + len;
    int sent = sendfull(fd, reinterpret_cast<const char*>(resp), totalLen, 0);
    //std::cerr << "length is " <<sent <<std::endl;
    assert(sent == totalLen);

    ++finishedReqs;
    //std::cerr << finishedReqs<<' '<<warmupReqs<<std::endl;
    if (finishedReqs == warmupReqs) {
        resp->type = ROI_BEGIN;
        for (int fd : clientFds) {
            totalLen = sizeof(Response) - MAX_RESP_BYTES;
            sent = sendfull(fd, reinterpret_cast<const char*>(resp), totalLen, 0);
            assert(sent == totalLen);
        }
    } else if (finishedReqs == warmupReqs + maxReqs) { 
        resp->type = FINISH;
        for (int fd : clientFds) {
            totalLen = sizeof(Response) - MAX_RESP_BYTES;
            sent = sendfull(fd, reinterpret_cast<const char*>(resp), totalLen, 0);
            assert(sent == totalLen);
        }
    }

    delete resp;
    
    pthread_mutex_unlock(&sendLock);
}

void NetworkedServer::finish() {
    pthread_mutex_lock(&sendLock);

    Response* resp = new Response();
    resp->type = FINISH;

    for (int fd : clientFds) {
        int len = sizeof(Response) - MAX_RESP_BYTES;
        int sent = sendfull(fd, reinterpret_cast<const char*>(resp), len, 0);
        assert(sent == len);
    }

    delete resp;
    
    pthread_mutex_unlock(&sendLock);
}

/*******************************************************************************
 * Per-thread State
 *******************************************************************************/
__thread int tid;

/*******************************************************************************
 * Global data
 *******************************************************************************/
std::atomic_int curTid;
NetworkedServer* server;

/*******************************************************************************
 * API
 *******************************************************************************/
void tBenchServerInit(int nthreads) {
    curTid = 0;
    std::string serverurl = getOpt<std::string>("TBENCH_SERVER", "");
    int serverport = getOpt<int>("TBENCH_SERVER_PORT", 7000);
    int nclients = getOpt<int>("TBENCH_NCLIENTS", 1);
    server = new NetworkedServer(nthreads, serverurl, serverport, nclients);
}

void tBenchServerThreadStart() {
    tid = curTid++;
}

void tBenchServerFinish() {
    server->finish();
}

size_t tBenchRecvReq(void** data) {
    return server->recvReq(tid, data);
}

void tBenchSendResp(const void* data, size_t size) {
    return server->sendResp(tid, data, size);
}


void* receive_thread_func(void *ptr)
{   
    NetworkedServer *server = (NetworkedServer*)ptr;
    int ret_val = 1;

    while(ret_val) 
    {
        ret_val = server->recvReq_Q();
    }

    return 0;
}

void tBenchSetup_thread()
{
    receive_thread = new pthread_t;
    attr = new pthread_attr_t;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(12,&cpuset);
    pthread_attr_init(attr);
    pthread_attr_setaffinity_np(attr,sizeof(cpu_set_t),&cpuset);
    pthread_create(receive_thread, attr, receive_thread_func, (void *)server);
}


void tBench_deleteReq() 
{
    delete global_req;
}

void tBench_join() 
{   
    pthread_join(*receive_thread,NULL);
}
