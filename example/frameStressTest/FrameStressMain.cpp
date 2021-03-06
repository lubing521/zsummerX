﻿/*
 * zsummerX License
 * -----------
 * 
 * zsummerX is licensed under the terms of the MIT license reproduced below.
 * This means that zsummerX is free software and can be used for both academic
 * and commercial purposes at absolutely no cost.
 * 
 * 
 * ===============================================================================
 * 
 * Copyright (C) 2010-2014 YaweiZhang <yawei_zhang@foxmail.com>.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 * 
 * ===============================================================================
 * 
 * (end of COPYRIGHT)
 */


//! frame module stress test
//! frame module used 'single sodule', if you need use multi-thread then you can create thread at user-code-layer, and you can look the example 'tcpStressTest'.

#include <zsummerX/zsummerX.h>
#include <unordered_map>
#include "TestProto.h"
using namespace zsummer::log4z;
using namespace zsummer::proto4z;
using namespace zsummer::network;


//! default value
std::string g_remoteIP = "0.0.0.0"; 
unsigned short g_remotePort = 8081; 
unsigned short g_startType = 0;  //0 start with server module, 1 start with client module. 
unsigned short g_maxClient = 100; //if start with server it is client limite count. else other it is the start client count.
unsigned short g_sendType = 0; //0 ping-pong test, 1 flooding test
unsigned int   g_intervalMs = 0; // if start client with flood test, it's flooding interval by millionsecond.



//! send this data by test
std::string g_testStr;

//!statistic 
unsigned long long g_totalEchoCount = 0;
unsigned long long g_lastEchoCount = 0;
unsigned long long g_totalSendCount = 0;
unsigned long long g_totalRecvCount = 0;
//!statistic monitor
void MonitorFunc()
{
	LOGI("per seconds Echos Count=" << (g_totalEchoCount - g_lastEchoCount) / 5
		<< ", g_totalSendCount[" << g_totalSendCount << "] g_totalRecvCount[" << g_totalRecvCount << "]");
	g_lastEchoCount = g_totalEchoCount;
	SessionManager::getRef().createTimer(5000, MonitorFunc);
};


//make heartbeat mechanisms.
//mechanisms: register pulse timer, send pulse proto when timer trigger.  check last remote pulse time when trigger.
class CHeartbeatManager
{
public:
	CHeartbeatManager()
	{
		MessageDispatcher::getRef().registerOnSessionEstablished(std::bind(&CHeartbeatManager::OnSessionEstablished, this,
			std::placeholders::_1));
		MessageDispatcher::getRef().registerOnSessionDisconnect(std::bind(&CHeartbeatManager::OnSessionDisconnect, this,
			std::placeholders::_1));
		MessageDispatcher::getRef().registerOnSessionPulse(std::bind(&CHeartbeatManager::OnSessionPulse, this,
			std::placeholders::_1, std::placeholders::_2));
		MessageDispatcher::getRef().registerSessionMessage(ID_C2S_Pulse, std::bind(&CHeartbeatManager::OnMsgPulse, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
		MessageDispatcher::getRef().registerSessionMessage(ID_S2C_Pulse, std::bind(&CHeartbeatManager::OnMsgPulse, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
	}
	

	void OnMsgPulse(SessionID sID, ProtoID pID, ReadStream & pack)
	{
		auto iter = _sessionPulse.find(sID);
		if (iter != _sessionPulse.end())
		{
			iter->second = time(NULL);
		}
	}

	void OnSessionEstablished(SessionID sID)
	{
		_sessionPulse[sID] = time(NULL);
		LOGI("OnSessionEstablished. sID=" << sID);
	}

	void OnSessionPulse(SessionID sID, unsigned int pulseInterval)
	{
		auto iter = _sessionPulse.find(sID);
		if (iter == _sessionPulse.end())
		{
			LOGI("remote session lost. sID=" << sID );
			SessionManager::getRef().kickSession(sID);
			return;
		}
		if (time(NULL) - iter->second > pulseInterval / 1000 * 2)
		{
			LOGI("remote session timeout. sID=" << sID << ", timeout=" << time(NULL) - iter->second);
			SessionManager::getRef().kickSession(sID);
			return;
		}

		if (isConnectID(sID))
		{
			WriteStream pack(ID_C2S_Pulse);
			SessionManager::getRef().sendOrgSessionData(sID, pack.getStream(), pack.getStreamLen());
		}
		else
		{
			WriteStream pack(ID_S2C_Pulse);
			SessionManager::getRef().sendOrgSessionData(sID, pack.getStream(), pack.getStreamLen());
		}
	}
	void OnSessionDisconnect(SessionID sID)
	{
		LOGI("OnSessionDisconnect. sID=" << sID );
		_sessionPulse.erase(sID);
	}
private:
	std::unordered_map<SessionID, time_t> _sessionPulse;
};

//client handler
// ping-pong module test: 
//			1. send first message to server when client connect server success.
//			2. send message to server again when receive server echo.
// flooding module test:
//			1. create a timer used to send one message to server when client connect server success.
//			2. create a timer used to send next message again when the last timer trigger and send message.

class CStressClientHandler
{
public:
	CStressClientHandler()
	{
		MessageDispatcher::getRef().registerOnSessionEstablished(std::bind(&CStressClientHandler::onConnected, this, std::placeholders::_1));
		MessageDispatcher::getRef().registerSessionMessage(ID_P2P_EchoPack,
			std::bind(&CStressClientHandler::msg_ResultSequence_fun, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
		MessageDispatcher::getRef().registerSessionMessage(ID_P2P_EchoPack,
			std::bind(&CStressClientHandler::looker_ResultSequence_fun, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
		MessageDispatcher::getRef().registerOnSessionDisconnect(std::bind(&CStressClientHandler::OnConnectDisconnect, this, std::placeholders::_1));
	}

	void onConnected (SessionID cID)
	{
		LOGI("onConnected. ConnectorID=" << cID );
		WriteStream ws(ID_P2P_EchoPack);
		P2P_EchoPack pack;
		TestIntegerData idata;
		idata._char = 'a';
		idata._uchar = 100;
		idata._short = 200;
		idata._ushort = 300;
		idata._int = 400;
		idata._uint = 500;
		idata._i64 = 600;
		idata._ui64 = 700;
		idata._ui128 = 255;

		TestFloatData fdata;
		fdata._float = (float)123123.111111;
		fdata._double = 12312312.88888;

		TestStringData sdata;
		sdata._string = "abcdefg";

		pack._iarray.push_back(idata);
		pack._iarray.push_back(idata);
		pack._farray.push_back(fdata);
		pack._farray.push_back(fdata);
		pack._sarray.push_back(sdata);
		pack._sarray.push_back(sdata);

		pack._imap.insert(std::make_pair("123", idata));
		pack._imap.insert(std::make_pair("223", idata));
		pack._fmap.insert(std::make_pair("323", fdata));
		pack._fmap.insert(std::make_pair("423", fdata));
		pack._smap.insert(std::make_pair("523", sdata));
		pack._smap.insert(std::make_pair("623", sdata));
		ws << pack;
		SessionManager::getRef().sendOrgSessionData(cID, ws.getStream(), ws.getStreamLen());
		g_totalSendCount++;
		if (g_sendType != 0 && g_intervalMs > 0)
		{
			SessionManager::getRef().createTimer(g_intervalMs, std::bind(&CStressClientHandler::SendFunc, this, cID));
			_sessionStatus[cID] = true;
		}
	};
	void OnConnectDisconnect(SessionID cID)
	{
		_sessionStatus[cID] = false;
	}

	void msg_ResultSequence_fun(SessionID cID, ProtoID pID, ReadStream & rs)
	{
		P2P_EchoPack pack;
		rs >> pack;

		g_totalRecvCount++;
		g_totalEchoCount++;

		if (g_sendType == 0 || g_intervalMs == 0) //echo send
		{
			WriteStream ws(ID_P2P_EchoPack);
			ws << pack;
			SessionManager::getRef().sendOrgSessionData(cID, ws.getStream(), ws.getStreamLen());
			g_totalSendCount++;
		}
	};
	void looker_ResultSequence_fun(SessionID cID, ProtoID pID, ReadStream & rs)
	{
//		P2P_EchoPack pack;
//		rs >> pack;
//		LOGA("looker:" <<msg);
	};
	void SendFunc(SessionID cID)
	{
		if (g_totalSendCount - g_totalRecvCount < 10000)
		{
			WriteStream ws(ID_P2P_EchoPack);
			P2P_EchoPack pack;
			TestIntegerData idata;
			idata._char = 'a';
			idata._uchar = 100;
			idata._short = 200;
			idata._ushort = 300;
			idata._int = 400;
			idata._uint = 500;
			idata._i64 = 600;
			idata._ui64 = 700;
			idata._ui128 = 255;

			TestFloatData fdata;
			fdata._float = (float)123123.111111;
			fdata._double = 12312312.88888;

			TestStringData sdata;
			sdata._string = "abcdefg";

			pack._iarray.push_back(idata);
			pack._iarray.push_back(idata);
			pack._farray.push_back(fdata);
			pack._farray.push_back(fdata);
			pack._sarray.push_back(sdata);
			pack._sarray.push_back(sdata);

			pack._imap.insert(std::make_pair("123", idata));
			pack._imap.insert(std::make_pair("223", idata));
			pack._fmap.insert(std::make_pair("323", fdata));
			pack._fmap.insert(std::make_pair("423", fdata));
			pack._smap.insert(std::make_pair("523", sdata));
			pack._smap.insert(std::make_pair("623", sdata));
			ws << pack;
			SessionManager::getRef().sendOrgSessionData(cID, ws.getStream(), ws.getStreamLen());
			g_totalSendCount++;
		}
		if (_sessionStatus[cID])
		{
			SessionManager::getRef().createTimer(g_intervalMs, std::bind(&CStressClientHandler::SendFunc, this, cID));
		}
	};
private:
	std::unordered_map<SessionID, bool> _sessionStatus;
};


/*
* server handler
* echo data when receive client message.
*/
class CStressServerHandler
{
public:
	CStressServerHandler()
	{
		MessageDispatcher::getRef().registerSessionMessage(ID_P2P_EchoPack,
			std::bind(&CStressServerHandler::msg_RequestSequence_fun, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
	}

	void msg_RequestSequence_fun (SessionID sID, ProtoID pID, ReadStream & rs)
	{
		P2P_EchoPack pack;
		rs >> pack;
		WriteStream ws(ID_P2P_EchoPack);
		ws << pack;
		SessionManager::getRef().sendOrgSessionData(sID, ws.getStream(), ws.getStreamLen());
		g_totalEchoCount++;
		g_totalSendCount++;
		g_totalRecvCount++;
	};
};


void sigFun(int sig)
{
	SessionManager::getRef().stop();
}

int main(int argc, char* argv[])
{

#ifndef _WIN32
	//! ignore some signal
	signal( SIGHUP, SIG_IGN );
	signal( SIGALRM, SIG_IGN ); 
	signal( SIGPIPE, SIG_IGN );
	signal( SIGXCPU, SIG_IGN );
	signal( SIGXFSZ, SIG_IGN );
	signal( SIGPROF, SIG_IGN ); 
	signal( SIGVTALRM, SIG_IGN );
	signal( SIGQUIT, SIG_IGN );
	signal( SIGCHLD, SIG_IGN);
#endif
	signal(SIGINT, sigFun);
	if (argc == 2 && 
		(strcmp(argv[1], "--help") == 0 
		|| strcmp(argv[1], "/?") == 0))
	{
		std::cout << "please input like example:" << std::endl;
		std::cout << "tcpTest remoteIP remotePort startType maxClient sendType interval" << std::endl;
		std::cout << "./tcpTest 0.0.0.0 8081 0" << std::endl;
		std::cout << "startType: 0 server, 1 client" << std::endl;
		std::cout << "maxClient: limit max" << std::endl;
		std::cout << "sendType: 0 echo send, 1 direct send" << std::endl;
		std::cout << "interval: send once interval" << std::endl;
		return 0;
	}
	if (argc > 1)
		g_remoteIP = argv[1];
	if (argc > 2)
		g_remotePort = atoi(argv[2]);
	if (argc > 3)
		g_startType = atoi(argv[3]);
		if (g_startType != 0) g_maxClient = 1;
	if (argc > 4)
		g_maxClient = atoi(argv[4]);
	if (argc > 5)
		g_sendType = atoi(argv[5]);
	if (argc > 6)
		g_intervalMs = atoi(argv[6]);

	if (g_startType == 0)
		ILog4zManager::getPtr()->config("server.cfg");
	else
		ILog4zManager::getPtr()->config("client.cfg");
	ILog4zManager::getPtr()->start();

	LOGI("g_remoteIP=" << g_remoteIP << ", g_remotePort=" << g_remotePort << ", g_startType=" << g_startType
		<< ", g_maxClient=" << g_maxClient << ", g_sendType=" << g_sendType << ", g_intervalMs=" << g_intervalMs);


	ILog4zManager::getPtr()->setLoggerLevel(LOG4Z_MAIN_LOGGER_ID, LOG_LEVEL_INFO);



	SessionManager::getRef().start();

	g_testStr.resize(200, 's');

	SessionManager::getRef().createTimer(5000, MonitorFunc);

	//build heartbeat manager
	CHeartbeatManager heartbeatManager;

	//build client and server
	if (g_startType) //client
	{
		CStressClientHandler client;
		//add connector.
		for (int i = 0; i < g_maxClient; ++i)
		{
			ConnectConfig traits;
			traits._remoteIP = g_remoteIP;
			traits._remotePort = g_remotePort;
			traits._reconnectInterval = 5000;
			traits._reconnectMaxCount = 5;
//			traits._rc4TcpEncryption = "yawei.zhang@foxmail.com";
			traits._pulseInterval = 500000;
			SessionManager::getRef().addConnector(traits);
		}
		//running
		SessionManager::getRef().run();
	}
	else
	{
		CStressServerHandler server;
		//add acceptor
		ListenConfig traits;
		traits._listenPort = g_remotePort;
		traits._maxSessions = g_maxClient;
//		traits._rc4TcpEncryption = "yawei.zhang@foxmail.com";
		//traits._openFlashPolicy = true;
		traits._pulseInterval = 500000;
		//traits._whitelistIP.push_back("127.0.");
		SessionManager::getRef().addAcceptor(traits);
		//running
		SessionManager::getRef().run();
	}



	return 0;
}

