#include "IOCPModel.h"

#define WORKER_THREADS_PER_PROCESSOR 2 // 每个处理器上产生多少个线程
#define MAX_POST_ACCEPT 10 // 同时投递的Accept请求数量（这个要根据实际情况灵活设置）
#define EXIT_CODE NULL // 传递给Worker线程的退出信号

// 释放指针和句柄资源的宏
#define RELEASE(x) {if(x != NULL){delete x;x=NULL;}} // 释放指针宏
#define RELEASE_HANDLE(x) {if(x != NULL && x != INVALID_HANDLE_VALUE){CloseHandle(x);x = NULL;}} // 释放句柄宏
#define RELEASE_SOCKET(x) {if(x != INVALID_SOCKET){closesocket(x);x = INVALID_SOCKET;}} // 释放socket宏

CIOCPModel::CIOCPModel():
						m_nThreads(0),
						m_hShutdownEvent(NULL),
						m_hIOCompletionPort(NULL),
						m_phWorkerThreads(NULL),
						m_strIP(DEFAULT_IP),
						m_nPort(DEFAULT_PORT),
						m_lpfnAcceptEx(NULL),
						m_lpfnGetAcceptExSockAddrs(NULL)
{

}

CIOCPModel::~CIOCPModel()
{
	this->Stop(); // 确保资源彻底释放
}

// 工作者线程：为IOCP请求服务的工作者线程 随时处理已有数据报的线程
DWORD WINAPI CIOCPModel::_WorkerThread(LPVOID lpParam)
{
	THREADPARAMS_WORKER* pParam = (THREADPARAMS_WORKER*)lpParam;
	CIOCPModel* pIOCPModel = (CIOCPModel*)pParam->pIOCPModel;
	int nThreadNo = pParam->nThreadNo;
	printf_s("工作者线程启动,ID:%d.\n",nThreadNo);

	OVERLAPPED* pOverlapped = NULL;
	PER_SOCKET_CONTEXT* pSocketContext = NULL;
	DWORD dwBytesTransfered = 0;

	// 循环处理请求，直道接收到Shutdown信息为之
	while (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPModel->m_hShutdownEvent, 0))
	{
		BOOL bReturn = GetQueuedCompletionStatus(
			pIOCPModel->m_hIOCompletionPort,
			&dwBytesTransfered,
			(PULONG_PTR)&pSocketContext,
			&pOverlapped,
			INFINITE);

		if (EXIT_CODE == (DWORD)pSocketContext) // 如果接受到的是退出标志，则直接退出
		{
			break;
		}

		if (!bReturn) // 是否出现错误
		{
			DWORD dwErr = GetLastError();
			if (!pIOCPModel->_HandleError(pSocketContext,dwErr))
			{
				break;
			}
			continue;
		}
		else
		{
			PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped,PER_IO_CONTEXT,m_overlapped);

			if ((0 == dwBytesTransfered) && (RECV_POSTED == pIoContext->m_opType || SEND_POSTED == pIoContext->m_opType))
			{
				printf_s("客户端%s:%d断开连接.",inet_ntoa(pSocketContext->m_clientAddr.sin_addr),ntohs(pSocketContext->m_clientAddr.sin_port));
				pIOCPModel->_RemoveContext(pSocketContext); // 释放掉对应的资源
				continue;
			}
			else
			{
				switch (pIoContext->m_opType)
				{
				case ACCEPT_POSTED:
					{
						pIOCPModel->_DoAccept(pSocketContext,pIoContext);
						break;
					}
				case RECV_POSTED:
					{
						pIOCPModel->_DoRecv(pSocketContext,pIoContext);
						break;
					}
				case SEND_POSTED:
					{
						// 这里没有写
						break;
					}
				default:
					{
						printf_s("_WorkThread中的pIoContext->m_opType参数异常.\n");
						break;
					}
				}
			}
		}
	}
	printf_s("工作者线程%d号退出.\n",nThreadNo);

	RELEASE(lpParam); // 释放线程参数
	return 0;
}

// 初始化WinSock2.2
bool CIOCPModel::LoadSocketLib()
{
	WSADATA wsaData;
	int nResult = WSAStartup(MAKEWORD(2,2),&wsaData);
	if (NO_ERROR != nResult)
	{
		printf_s("初始化WinSock2.2失败！\n");
		return false;
	}
	return true;
}

// 启动服务器
bool CIOCPModel::Start()
{
	InitializeCriticalSection(&m_csContextList); // 初始化线程互斥量

	// 信号的使用
	// 一个Event被创建以后,可以用OpenEvent()API来获得它的Handle,用CloseHandle()来关闭它,
	// 用SetEvent()或PulseEvent()来设置它使其有信号,用ResetEvent()来使其无信号,
	// 用WaitForSingleObject()或WaitForMultipleObjects()来等待其变为有信号
	// lpEventAttributes 一般为NULL
	// bManualReset 创建的Event是自动复位还是人工复位.如果true,人工复位,一旦该Event被设置为有信号,则它一直会等到ResetEvent()API被调用时才会恢复为无信号.
	// bInitialState 初始状态,true,有信号,false无信号
	// lpName 事件对象的名称。您在OpenEvent函数中可能使用。
	m_hShutdownEvent = CreateEvent(NULL,TRUE,FALSE,NULL); // 建立系统退出的事件通知

	if (!_InitializeIOCP()) // 初始化IOCP
	{
		printf_s("初始化IOCP失败\n");
		return false;
	}
	else
	{
		printf_s("IOCP初始化完毕\n");
	}

	if (!_InitializeListenSocket()) // 初始化socket
	{
		printf_s("Listen Socket初始化失败\n");
		_DeInitialize();
		return false;
	}
	else
	{
		printf_s("Listen Socket 初始化完毕\n");
	}

	printf_s("系统准备就绪，等待连接\n");
	return true;
}

// 开始发送系统退出消息，退出完成端口和线程资源
void CIOCPModel::Stop()
{
	if ((m_pListenContext != NULL) && (m_pListenContext->m_sock != INVALID_SOCKET))
	{
		SetEvent(m_hShutdownEvent); // 激活关闭消息通知

		for (int i = 0; i < m_nThreads; ++i)
		{
			PostQueuedCompletionStatus(m_hIOCompletionPort,0,(DWORD)EXIT_CODE,NULL); // 通知所有完成端口操作退出
		}

		WaitForMultipleObjects(m_nThreads,m_phWorkerThreads,TRUE,INFINITE); // 等待所有客户端资源退出

		this->_ClearContextList(); // 清除客户端列表信息

		this->_DeInitialize(); // 释放其他资源

		printf_s("停止监听\n");
	}
}

// 初始化完成端口
bool CIOCPModel::_InitializeIOCP()
{
	m_hIOCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE,NULL,0,0); // 建立第一个完成端口

	if (NULL == m_hIOCompletionPort)
	{
		printf("建立完成端口失败，错误码：%d\n",WSAGetLastError());
		return false;
	}

	m_nThreads = WORKER_THREADS_PER_PROCESSOR * _GetNoOfProcessors(); // 根据本机中的处理器数量，建立对应的线程数

	m_phWorkerThreads = new HANDLE[m_nThreads]; // 为工作者线程初始化句柄

	// 根据计算的数量建立工作者线程
	DWORD nThreadID;
	for (int i = 0; i < m_nThreads; ++i)
	{
		THREADPARAMS_WORKER* pThreadParams = new THREADPARAMS_WORKER;
		pThreadParams->pIOCPModel = this;
		pThreadParams->nThreadNo = i+1;
		m_phWorkerThreads[i] = ::CreateThread(0,0,_WorkerThread,(void*)pThreadParams,0,&nThreadID);
	}

	printf_s("建立_WorkerThread %d个.\n",m_nThreads);

	return true;
}

// 初始化socket
bool CIOCPModel::_InitializeListenSocket()
{
	// AcceptEx和GetAcceptExSockAddrs的Guid，用于导出函数指针
	GUID guidAcceptEx = WSAID_ACCEPTEX;
	GUID guidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;

	sockaddr_in serverAddr; // 服务器地址信息

	m_pListenContext = new PER_SOCKET_CONTEXT; // 生成用于监听的socket的信息

	// 需要使用重叠IO，必须的使用WSASocket来建立socket，才可以支持重叠IO操作
	m_pListenContext->m_sock = WSASocket(AF_INET,SOCK_STREAM,0,NULL,0,WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == m_pListenContext->m_sock)
	{
		printf_s("初始化socket失败，错误码：%d.\n",WSAGetLastError());
		return false;
	}
	else
	{
		printf_s("WSASocket完成.\n");
	}

	// 将Listen Socket绑定到完成端口中
	if (NULL == CreateIoCompletionPort((HANDLE)m_pListenContext->m_sock,m_hIOCompletionPort,(DWORD)m_pListenContext,0))
	{
		printf_s("绑定Listen Socket至完成端口失败,错误码:%d\n",WSAGetLastError());
		return false;
	}
	else
	{
		printf_s("Listen Socket绑定完成端口完成.\n");
	}

	// 填充地址信息
	ZeroMemory((char*)&serverAddr,sizeof(sockaddr_in));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY); // 绑定任何可用的ip
	//serverAddr.sin_addr.s_addr = inet_addr(m_strIP.GetString()); // 绑定指定的ip
	serverAddr.sin_port = htons(m_nPort);

	// 绑定地址和端口
	if (SOCKET_ERROR == bind(m_pListenContext->m_sock,(sockaddr*)&serverAddr,sizeof(serverAddr)))
	{
		printf_s("bind()函数执行错误.\n");
		return false;
	}
	else
	{
		printf_s("bind()完成\n");
	}

	// 开始监听
	if (SOCKET_ERROR == listen(m_pListenContext->m_sock,SOMAXCONN))
	{
		printf_s("listen()函数执行错误.\n");
		return false;
	}
	else
	{
		printf_s("listen()完成\n");
	}

	// 使用AcceptEx函数，因为这个是属于WinSock2规范之外的微软另外提供的扩展函数
	// 所以需要额外获取一下函数的指针，
	// 获取AcceptEx函数指针
	DWORD dwBytes = 0;
	if (SOCKET_ERROR == WSAIoctl(
		m_pListenContext->m_sock,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidAcceptEx,
		sizeof(guidAcceptEx),
		&m_lpfnAcceptEx,
		sizeof(m_lpfnAcceptEx),
		&dwBytes,
		NULL,
		NULL
		))
	{
		printf_s("WSAIoctl未能获取AcceptEx函数指针，错误码：%d.\n",WSAGetLastError());
		this->_DeInitialize();
		return false;
	}

	// 获取GetAcceptExSockAddrs函数指针，也是同理
	if (SOCKET_ERROR == WSAIoctl(
		m_pListenContext->m_sock,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidGetAcceptExSockAddrs,
		sizeof(guidGetAcceptExSockAddrs),
		&m_lpfnGetAcceptExSockAddrs,
		sizeof(m_lpfnGetAcceptExSockAddrs),
		&dwBytes,
		NULL,
		NULL
		))
	{
		printf_s("WSAIoctl未能获取GetAcceptExSockAddrs函数指针，错误码：%d.\n",WSAGetLastError());
		this->_DeInitialize();
		return false;
	}

	// 为AcceptEx准备参数，然后投递AcceptEx I/O请求
	for (int i = 0; i < MAX_POST_ACCEPT; ++i)
	{
		PER_IO_CONTEXT* pAcceptIoContext = m_pListenContext->GetNewIoContext();
		if (!this->_PostAccept(pAcceptIoContext))
		{
			m_pListenContext->RemoveContext(pAcceptIoContext);
			return false;
		}
	}

	printf_s("投递%d个AcceptEx请求完毕.\n",MAX_POST_ACCEPT);

	return true;
}

// 最后释放掉所有资源
void CIOCPModel::_DeInitialize()
{
	DeleteCriticalSection(&m_csContextList); // 删除客户端列表的互斥量

	RELEASE_HANDLE(m_hShutdownEvent); // 关闭系统退出事件句柄

	// 释放工作者线程句柄指针
	for (int i = 0; i < m_nThreads; ++i)
	{
		RELEASE_HANDLE(m_phWorkerThreads[i]);
	}

	RELEASE(m_phWorkerThreads);

	RELEASE_HANDLE(m_hIOCompletionPort); // 关闭IOCP句柄

	RELEASE(m_pListenContext); // 关闭监听socket的context信息

	printf_s("释放资源完毕.\n");
}

// 投递Accept请求
bool CIOCPModel::_PostAccept(PER_IO_CONTEXT* pAcceptIoContext)
{
	CHECK_BOOL_BOOL((INVALID_SOCKET != m_pListenContext->m_sock),"_PostAccept无效的socket\n");

	// 准备参数
	DWORD dwBytes = 0;
	pAcceptIoContext->m_opType = ACCEPT_POSTED;
	WSABUF* p_wbuf = &pAcceptIoContext->m_wsaBuf;
	OVERLAPPED* p_ol = &pAcceptIoContext->m_overlapped;

	// 为以后新连入的客户端准备好socket（这是与传统Accept的最大区别）
	pAcceptIoContext->m_sock = WSASocket(AF_INET,SOCK_STREAM,IPPROTO_TCP,NULL,0,WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == pAcceptIoContext->m_sock)
	{
		printf_s("创建用于accept的socket失败，错误码：%d.\n",WSAGetLastError());
		return false;
	}

	// 投递AcceptEx
	if (FALSE == m_lpfnAcceptEx(m_pListenContext->m_sock,pAcceptIoContext->m_sock,p_wbuf->buf,p_wbuf->len-((sizeof(SOCKADDR_IN)+16)*2),
		sizeof(SOCKADDR_IN)+16,sizeof(SOCKADDR_IN)+16,&dwBytes,p_ol))
	{
		if (WSA_IO_PENDING != WSAGetLastError())
		{
			printf_s("投递AcceptEx请求失败，错误码：%d.\n",WSAGetLastError());
			return false;
		}
	}

	return true;
}

// 处理Accept请求（客户端连入时，进行处理）
bool CIOCPModel::_DoAccept(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext)
{
	SOCKADDR_IN* pCliendAddr = NULL;
	SOCKADDR_IN* pLocalAddr = NULL;
	int remoteLen = sizeof(SOCKADDR_IN), localLen = sizeof(SOCKADDR_IN);

	// 获取客户端、本地地址信息，同时获取客户端发来的第一组数据
	this->m_lpfnGetAcceptExSockAddrs(pIoContext->m_wsaBuf.buf,pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN)+16)*2),
		sizeof(SOCKADDR_IN)+16,sizeof(SOCKADDR_IN)+16,(LPSOCKADDR*)&pLocalAddr,&localLen,(LPSOCKADDR*)&pCliendAddr,&remoteLen);

	printf_s("客户端%s:%d连入.\n",inet_ntoa(pCliendAddr->sin_addr),ntohs(pCliendAddr->sin_port));
	printf_s("客户端%s:%d信息:%s.\n",inet_ntoa(pCliendAddr->sin_addr),ntohs(pCliendAddr->sin_port),pIoContext->m_wsaBuf.buf);

	// pSocketContext是listenSocket上的context，还需要监听下一个连接，所以需要新建一个socketContext对应新连入的socket
	PER_SOCKET_CONTEXT* pNewSocketContext = new PER_SOCKET_CONTEXT;
	pNewSocketContext->m_sock = pIoContext->m_sock;
	memcpy(&(pNewSocketContext->m_clientAddr),pCliendAddr,sizeof(SOCKADDR_IN));

	// 将这个新socket和完成端口绑定
	if (!this->_AssociateWithIOCP(pNewSocketContext))
	{
		RELEASE(pNewSocketContext);
		return false;
	}

	// 建立socketContext下的ioContext用于在这个socket上投递的第一个Recv请求
	PER_IO_CONTEXT* pNewIoContext = pNewSocketContext->GetNewIoContext();
	pNewIoContext->m_opType = RECV_POSTED;
	pNewIoContext->m_sock = pNewSocketContext->m_sock;
	//memcpy(pNewIoContext->m_szBuffer,pIoContext->m_szBuffer,MAX_BUFFER_LEN); // 如果Buffer需要保留，就复制出来

	// 绑定完毕后，就可以就可以在这个socket上投递请求了
	if (!this->_PostRecv(pNewIoContext))
	{
		pNewSocketContext->RemoveContext(pNewIoContext);
		return false;
	}

	// 如果投递成功，就把这个有效客户端信息添加到contextList中统一管理
	this->_AddToContextList(pNewSocketContext);

	// 将listenSocket的ioContext重置，准备投递新的acceptEx
	pIoContext->ResetBuffer();
	return this->_PostAccept(pIoContext);
}

// 投递接收数据请求(初始化用，并不是真正的处理客户端数据)
bool CIOCPModel::_PostRecv(PER_IO_CONTEXT* pIoContext)
{
	// 初始化变量
	DWORD dwFlags = 0;
	DWORD dwBytes = 0;
	WSABUF* p_wbuf = &pIoContext->m_wsaBuf;
	OVERLAPPED* p_ol = &pIoContext->m_overlapped;

	pIoContext->ResetBuffer();
	pIoContext->m_opType = RECV_POSTED;

	// 初始化完毕后，投递WSARecv请求
	int nBytesRecv = WSARecv(pIoContext->m_sock,p_wbuf,1,&dwBytes,&dwFlags,p_ol,NULL);
	if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError())) // 返回值错误，并且错误码不是pending的话，就说明重叠请求失败了
	{
		printf_s("投递第一个WSARecv失败.\n");
		return false;
	}
	return true;
}

// 再有数据到达时，进行处理
bool CIOCPModel::_DoRecv(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext)
{
	// 取数据
	SOCKADDR_IN* pClientAddr = &pSocketContext->m_clientAddr;
	printf_s("收到%s:%d信息:%s\n",inet_ntoa(pClientAddr->sin_addr),ntohs(pClientAddr->sin_port),pIoContext->m_wsaBuf.buf);

	// 然后投递下一个WSARecv请求
	return _PostRecv(pIoContext);
}

// 将句柄(socket)绑定到完成端口中
bool CIOCPModel::_AssociateWithIOCP(PER_SOCKET_CONTEXT* pContext)
{
	HANDLE hTemp = CreateIoCompletionPort((HANDLE)pContext->m_sock,m_hIOCompletionPort,(DWORD)pContext,0);

	if (hTemp == NULL)
	{
		printf_s("执行CreateIoCompletionPort()出现错误，错误码:%d.",GetLastError());
		return false;
	}

	return true;
}

// 将客户端相关信息存储到数组中
void CIOCPModel::_AddToContextList(PER_SOCKET_CONTEXT* pSocketContext)
{
	EnterCriticalSection(&m_csContextList);

	m_arrayClientContext.push_back(pSocketContext);

	LeaveCriticalSection(&m_csContextList);
}

// 移除某个特定的Context
void CIOCPModel::_RemoveContext(PER_SOCKET_CONTEXT* pSocketContext)
{
	EnterCriticalSection(&m_csContextList);

	for (vector<PER_SOCKET_CONTEXT*>::iterator it = m_arrayClientContext.begin(); it != m_arrayClientContext.end(); ++it)
	{
		if (pSocketContext == *it)
		{
			RELEASE(pSocketContext);
			m_arrayClientContext.erase(it);
			break;
		}
	}

	LeaveCriticalSection(&m_csContextList);
}

// 清空客户端信息
void CIOCPModel::_ClearContextList()
{
	EnterCriticalSection(&m_csContextList);

	for (vector<PER_SOCKET_CONTEXT*>::iterator it = m_arrayClientContext.begin(); it != m_arrayClientContext.end(); ++it)
	{
		delete *it;
	}
	m_arrayClientContext.clear();

	LeaveCriticalSection(&m_csContextList);
}

// 获取本机IP地址
string CIOCPModel::GetLocalIP()
{
	// 获得本机主机名
	char hostname[MAX_PATH] = {0};
	gethostname(hostname,MAX_PATH);
	hostent FAR* lpHostEnt = gethostbyname(hostname);
	if (lpHostEnt == NULL)
	{
		return DEFAULT_IP;
	}

	//  取得IP地址列表中的第一个返回的IP（因为一台主机可能会绑定多个IP）
	LPSTR lpAddr = lpHostEnt->h_addr_list[0];

	// 将IP地址转化成字符串形式
	in_addr inAddr;
	memmove(&inAddr,lpAddr,4);
	m_strIP = inet_ntoa(inAddr);
	return m_strIP;
}

// 获取本机中的处理器的数量
int CIOCPModel::_GetNoOfProcessors()
{
	SYSTEM_INFO si;

	GetSystemInfo(&si);

	return si.dwNumberOfProcessors;
}

// 判断客户端Socket是否已经断开，否则在一个无效的Socket上投递WSARecv操作会出现异常
// 使用的方法是尝试向这个socket发送数据，判断这个socket调用的返回值
// 因为如果客户端网络异常断开(例如客户端崩溃或者拔掉网线等)的时候，服务器端是无法收到客户端断开的通知的
bool CIOCPModel::_IsSocketAlive(SOCKET s)
{
	int nByteSend = send(s,"",0,0);
	if (-1 == nByteSend) return false;
	return true;
}

// 显示并处理完成端口的错误
bool CIOCPModel::_HandleError(PER_SOCKET_CONTEXT* pContext, const DWORD& dwError)
{
	// 如果是超时，就继续等待
	if (WAIT_TIMEOUT == dwError)
	{
		// 确认客户端是否还活着
		if (!_IsSocketAlive(pContext->m_sock))
		{
			printf_s("检测到客户端异常退出.");
			this->_RemoveContext(pContext);
			return true;
		}
		else
		{
			printf_s("网络操作超时，重试中……");
			return true;
		}
	}

	// 可能是客户端异常退出了
	else if (ERROR_NETNAME_DELETED == dwError)
	{
		printf_s("检测到客户端异常退出.");
		return true;
	}
	else
	{
		printf_s("完成端口操作出现错误，线程退出。错误码:%d.\n",dwError);
		return false;
	}
}
