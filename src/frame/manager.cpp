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


#include <zsummerX/frame/session.h>
#include <zsummerX/frame/manager.h>
#include <zsummerX/frame/dispatch.h>
using namespace zsummer::network;
using namespace zsummer::proto4z;



SessionManager & SessionManager::getRef()
{
	static SessionManager _manager;
	return _manager;
}
SessionManager::SessionManager()
{
	_summer = std::shared_ptr<zsummer::network::EventLoop>(new zsummer::network::EventLoop());
}

bool SessionManager::start()
{
	
	if (!_summer->initialize())
	{
		return false;
	}
	_running = true;
	return true;
}

void SessionManager::stop()
{
	post(std::bind(&SessionManager::safeStop, this));
}

void SessionManager::safeStop()
{
	_running = false;

	//at first close the session from accept.
	_onlineConnectCounts = 0;
	for (auto m : _mapTcpSessionPtr)
	{
		if (isConnectID(m.first))
		{
			_mapConnectorConfig[m.first].second._curReconnectCount = -1;
			_onlineConnectCounts++;
			continue;
		}
		m.second->close();
	}
	//if only have the connect session then close all.
	if (_mapTcpSessionPtr.size() == _onlineConnectCounts)
	{
		for (auto m : _mapTcpSessionPtr)
		{
			m.second->close();
		}
	}
}

void SessionManager::run()
{
	while (_running || !_mapTcpSessionPtr.empty())
	{
		_summer->runOnce();
	}
}
void SessionManager::runOnce(bool isImmediately)
{
	if (_running || !_mapTcpSessionPtr.empty())
	{
		_summer->runOnce(isImmediately);
	}
}



AccepterID SessionManager::addAcceptor(const ListenConfig &traits)
{
	_lastAcceptID++;
	auto & pairConfig = _mapAccepterConfig[_lastAcceptID];
	pairConfig.first = traits;
	pairConfig.second._aID = _lastAcceptID;

	TcpAcceptPtr accepter(new zsummer::network::TcpAccept());
	accepter->initialize(_summer);
	if (!accepter->openAccept(traits._listenIP.c_str(), traits._listenPort))
	{
		LCE("addAcceptor openAccept Failed. traits=" << traits);
		return InvalidAccepterID;
	}
	_mapAccepterPtr[_lastAcceptID] = accepter;
	TcpSocketPtr newclient(new zsummer::network::TcpSocket);
	accepter->doAccept(newclient, std::bind(&SessionManager::onAcceptNewClient, this, std::placeholders::_1, std::placeholders::_2, accepter, pairConfig.second._aID));
	return _lastAcceptID;
}

AccepterID SessionManager::getAccepterID(SessionID sID)
{
	if (!isSessionID(sID))
	{
		return InvalidAccepterID;
	}
	auto founder = _mapTcpSessionPtr.find(sID);
	if (founder == _mapTcpSessionPtr.end())
	{
		return InvalidAccepterID;
	}
	return founder->second->GetAcceptID();
}


void SessionManager::onAcceptNewClient(zsummer::network::ErrorCode ec, const TcpSocketPtr& s, const TcpAcceptPtr &accepter, AccepterID aID)
{
	if (!_running)
	{
		LCI("shutdown accepter. aID=" << aID);
		return;
	}
	auto iter = _mapAccepterConfig.find(aID);
	if (iter == _mapAccepterConfig.end())
	{
		LCE("Unknown AccepterID aID=" << aID);
		return;
	}
	if (ec)
	{
		LCE("doAccept Result Error. ec=" << ec << ", traits=" << iter->second.first);
		
		TcpSocketPtr newclient(new zsummer::network::TcpSocket);
		newclient->initialize(_summer);
		auto &&handler = std::bind(&SessionManager::onAcceptNewClient, this, std::placeholders::_1, std::placeholders::_2, accepter, aID);
		auto timer = [accepter, newclient, handler]()
		{
			accepter->doAccept(newclient, std::move(handler));
		};
		createTimer(5000, std::move(timer));
		return;
	}

	std::string remoteIP;
	unsigned short remotePort = 0;
	s->getPeerInfo(remoteIP, remotePort);
	
	//! check white list
	//! ---------------------
	if (!iter->second.first._whitelistIP.empty())
	{
		bool checkSucess = false;
		for (auto white : iter->second.first._whitelistIP)
		{
			if (remoteIP.size() >= white.size())
			{
				if (remoteIP.compare(0,white.size(), white) == 0)
				{
					checkSucess = true;
					break;
				}
			}
		}

		if (!checkSucess)
		{
			LCW("Accept New Client Check Whitelist Failed remoteAdress=" << remoteIP << ":" << remotePort
				<< ", trais=" << iter->second.first);

			TcpSocketPtr newclient(new zsummer::network::TcpSocket);
			newclient->initialize(_summer);
			accepter->doAccept(newclient, std::bind(&SessionManager::onAcceptNewClient, this, std::placeholders::_1, std::placeholders::_2, accepter, aID));
			return;
		}
		else
		{
			LCI("Accept New Client Check Whitelist Success remoteAdress=" << remoteIP << ":" << remotePort
				<< ", trais=" << iter->second.first);
		}
	}
	
	//! check Max Sessions

	if (iter->second.second._currentLinked >= iter->second.first._maxSessions)
	{
		LCW("Accept New Client. Too Many Sessions And The new socket will closed. remoteAddress=" << remoteIP << ":" << remotePort 
			<< ", Aready linked sessions = " << iter->second.second._currentLinked << ", trais=" << iter->second.first);
	}
	else
	{
		LCD("Accept New Client. Accept new Sessions. The new socket  remoteAddress=" << remoteIP << ":" << remotePort 
			<< ", Aready linked sessions = " << iter->second.second._currentLinked << ", trais=" << iter->second.first);
		iter->second.second._currentLinked++;
		iter->second.second._totalAcceptCount++;

		_lastSessionID = nextSessionID(_lastSessionID);
		TcpSessionPtr session(new TcpSession());
		s->initialize(_summer);
		if (session->bindTcpSocketPrt(s, aID, _lastSessionID, iter->second.first))
		{
			_mapTcpSessionPtr[_lastSessionID] = session;
			post(std::bind(&MessageDispatcher::dispatchOnSessionEstablished, &MessageDispatcher::getRef(), _lastSessionID));
		}
	}
	
	//! accept next socket.
	TcpSocketPtr newclient(new zsummer::network::TcpSocket);
	newclient->initialize(_summer);
	accepter->doAccept(newclient, std::bind(&SessionManager::onAcceptNewClient, this, std::placeholders::_1, std::placeholders::_2, accepter,aID));
}





void SessionManager::kickSession(SessionID sID)
{
	auto iter = _mapTcpSessionPtr.find(sID);
	if (iter == _mapTcpSessionPtr.end())
	{
		LCW("kickSession NOT FOUND SessionID. SessionID=" << sID);
		return;
	}
	iter->second->close();
}

void SessionManager::onSessionClose(AccepterID aID, SessionID sID)
{
	_mapAccepterConfig[aID].second._currentLinked--;
	_mapTcpSessionPtr.erase(sID);
	MessageDispatcher::getRef().dispatchOnSessionDisconnect(sID);
	if (!_running && _mapTcpSessionPtr.size() <= _onlineConnectCounts)
	{
		createTimer(1000, std::bind(&SessionManager::safeStop, SessionManager::getPtr()));
	}
}


SessionID SessionManager::addConnector(const ConnectConfig & traits)
{
	_lastConnectID = nextConnectID(_lastConnectID);
	auto & pairConfig = _mapConnectorConfig[_lastConnectID];
	pairConfig.first = traits;
	pairConfig.second._cID = _lastConnectID;
	TcpSocketPtr sockPtr(new zsummer::network::TcpSocket());
	sockPtr->initialize(_summer);
	TcpSessionPtr sessionPtr(new TcpSession());
	sessionPtr->bindTcpConnectorPtr(sockPtr, pairConfig);
	return _lastConnectID;
}


void SessionManager::onConnect(SessionID cID, bool bConnected, const TcpSessionPtr &session)
{
	auto config = _mapConnectorConfig.find(cID);
	if (config == _mapConnectorConfig.end())
	{
		LCE("Unkwon Connector. Not Found ConnectorID=" << cID);
		return;
	}
	if (bConnected)
	{
		_mapTcpSessionPtr[cID] = session;
		config->second.second._curReconnectCount = 0;
		post(std::bind(&MessageDispatcher::dispatchOnSessionEstablished, &MessageDispatcher::getRef(), cID));
		return;
	}


	auto iter = _mapTcpSessionPtr.find(cID);
	if (!bConnected && iter != _mapTcpSessionPtr.end())
	{
		_mapTcpSessionPtr.erase(cID);
		post(std::bind(&MessageDispatcher::dispatchOnSessionDisconnect, &MessageDispatcher::getRef(), cID));
	}

	if (!bConnected
		&& config->second.first._reconnectMaxCount > 0
		&& config->second.second._curReconnectCount < config->second.first._reconnectMaxCount)
	{
		config->second.second._curReconnectCount++;
		config->second.second._totalConnectCount++;

		TcpSocketPtr sockPtr(new zsummer::network::TcpSocket());
		sockPtr->initialize(_summer);
		createTimer(config->second.first._reconnectInterval, std::bind(&TcpSession::bindTcpConnectorPtr, session, sockPtr, config->second));
		LCW("Try reconnect current count=" << config->second.second._curReconnectCount << ", total reconnect = " << config->second.second._totalConnectCount << ". Traits=" << config->second.first);
		return;//try reconnect
	}
	
	if (config->second.first._reconnectMaxCount > 0 && _running)
	{
		LCE("Try Reconnect Failed. End Try. Traits=" << config->second.first);
	}
	//connect faild . clean data
	_mapConnectorConfig.erase(config);
}


void SessionManager::sendOrgSessionData(SessionID sID, const char * orgData, unsigned int orgDataLen)
{
	auto iter = _mapTcpSessionPtr.find(sID);
	if (iter == _mapTcpSessionPtr.end())
	{
		LCW("sendOrgSessionData NOT FOUND SessionID.  SessionID=" << sID);
		return;
	}
	iter->second->doSend(orgData, orgDataLen);
	//trace log
	{
		LCT("sendOrgSessionData Len=" << orgDataLen << ",binarydata=" << zsummer::log4z::Log4zBinary(orgData, orgDataLen >= 10 ? 10 : orgDataLen));
	}
}
void SessionManager::sendSessionData(SessionID sID, ProtoID pID, const char * userData, unsigned int userDataLen)
{
	WriteStream ws(pID);
	ws.appendOriginalData(userData, userDataLen);
	sendOrgSessionData(sID, ws.getStream(), ws.getStreamLen());
	//trace log
	{
		LCT("sendSessionData ProtoID=" << pID << ",  userDataLen=" << userDataLen);
	}
}



