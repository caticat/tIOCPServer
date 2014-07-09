#include "IOCPModel.h"

#define WORKER_THREADS_PER_PROCESSOR 2 // ÿ���������ϲ������ٸ��߳�
#define MAX_POST_ACCEPT 10 // ͬʱͶ�ݵ�Accept�������������Ҫ����ʵ�����������ã�
#define EXIT_CODE NULL // ���ݸ�Worker�̵߳��˳��ź�

// �ͷ�ָ��;����Դ�ĺ�
#define RELEASE(x) {if(x != NULL){delete x;x=NULL;}} // �ͷ�ָ���
#define RELEASE_HANDLE(x) {if(x != NULL && x != INVALID_HANDLE_VALUE){CloseHandle(x);x = NULL;}} // �ͷž����
#define RELEASE_SOCKET(x) {if(x != INVALID_SOCKET){closesocket(x);x = INVALID_SOCKET;}} // �ͷ�socket��

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
	this->Stop(); // ȷ����Դ�����ͷ�
}

// �������̣߳�ΪIOCP�������Ĺ������߳� ��ʱ�����������ݱ����߳�
DWORD WINAPI CIOCPModel::_WorkerThread(LPVOID lpParam)
{
	THREADPARAMS_WORKER* pParam = (THREADPARAMS_WORKER*)lpParam;
	CIOCPModel* pIOCPModel = (CIOCPModel*)pParam->pIOCPModel;
	int nThreadNo = pParam->nThreadNo;
	printf_s("�������߳�����,ID:%d.\n",nThreadNo);

	OVERLAPPED* pOverlapped = NULL;
	PER_SOCKET_CONTEXT* pSocketContext = NULL;
	DWORD dwBytesTransfered = 0;

	// ѭ����������ֱ�����յ�Shutdown��ϢΪ֮
	while (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPModel->m_hShutdownEvent, 0))
	{
		BOOL bReturn = GetQueuedCompletionStatus(
			pIOCPModel->m_hIOCompletionPort,
			&dwBytesTransfered,
			(PULONG_PTR)&pSocketContext,
			&pOverlapped,
			INFINITE);

		if (EXIT_CODE == (DWORD)pSocketContext) // ������ܵ������˳���־����ֱ���˳�
		{
			break;
		}

		if (!bReturn) // �Ƿ���ִ���
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
				printf_s("�ͻ���%s:%d�Ͽ�����.",inet_ntoa(pSocketContext->m_clientAddr.sin_addr),ntohs(pSocketContext->m_clientAddr.sin_port));
				pIOCPModel->_RemoveContext(pSocketContext); // �ͷŵ���Ӧ����Դ
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
						// ����û��д
						break;
					}
				default:
					{
						printf_s("_WorkThread�е�pIoContext->m_opType�����쳣.\n");
						break;
					}
				}
			}
		}
	}
	printf_s("�������߳�%d���˳�.\n",nThreadNo);

	RELEASE(lpParam); // �ͷ��̲߳���
	return 0;
}

// ��ʼ��WinSock2.2
bool CIOCPModel::LoadSocketLib()
{
	WSADATA wsaData;
	int nResult = WSAStartup(MAKEWORD(2,2),&wsaData);
	if (NO_ERROR != nResult)
	{
		printf_s("��ʼ��WinSock2.2ʧ�ܣ�\n");
		return false;
	}
	return true;
}

// ����������
bool CIOCPModel::Start()
{
	InitializeCriticalSection(&m_csContextList); // ��ʼ���̻߳�����

	// �źŵ�ʹ��
	// һ��Event�������Ժ�,������OpenEvent()API���������Handle,��CloseHandle()���ر���,
	// ��SetEvent()��PulseEvent()��������ʹ�����ź�,��ResetEvent()��ʹ�����ź�,
	// ��WaitForSingleObject()��WaitForMultipleObjects()���ȴ����Ϊ���ź�
	// lpEventAttributes һ��ΪNULL
	// bManualReset ������Event���Զ���λ�����˹���λ.���true,�˹���λ,һ����Event������Ϊ���ź�,����һֱ��ȵ�ResetEvent()API������ʱ�Ż�ָ�Ϊ���ź�.
	// bInitialState ��ʼ״̬,true,���ź�,false���ź�
	// lpName �¼���������ơ�����OpenEvent�����п���ʹ�á�
	m_hShutdownEvent = CreateEvent(NULL,TRUE,FALSE,NULL); // ����ϵͳ�˳����¼�֪ͨ

	if (!_InitializeIOCP()) // ��ʼ��IOCP
	{
		printf_s("��ʼ��IOCPʧ��\n");
		return false;
	}
	else
	{
		printf_s("IOCP��ʼ�����\n");
	}

	if (!_InitializeListenSocket()) // ��ʼ��socket
	{
		printf_s("Listen Socket��ʼ��ʧ��\n");
		_DeInitialize();
		return false;
	}
	else
	{
		printf_s("Listen Socket ��ʼ�����\n");
	}

	printf_s("ϵͳ׼���������ȴ�����\n");
	return true;
}

// ��ʼ����ϵͳ�˳���Ϣ���˳���ɶ˿ں��߳���Դ
void CIOCPModel::Stop()
{
	if ((m_pListenContext != NULL) && (m_pListenContext->m_sock != INVALID_SOCKET))
	{
		SetEvent(m_hShutdownEvent); // ����ر���Ϣ֪ͨ

		for (int i = 0; i < m_nThreads; ++i)
		{
			PostQueuedCompletionStatus(m_hIOCompletionPort,0,(DWORD)EXIT_CODE,NULL); // ֪ͨ������ɶ˿ڲ����˳�
		}

		WaitForMultipleObjects(m_nThreads,m_phWorkerThreads,TRUE,INFINITE); // �ȴ����пͻ�����Դ�˳�

		this->_ClearContextList(); // ����ͻ����б���Ϣ

		this->_DeInitialize(); // �ͷ�������Դ

		printf_s("ֹͣ����\n");
	}
}

// ��ʼ����ɶ˿�
bool CIOCPModel::_InitializeIOCP()
{
	m_hIOCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE,NULL,0,0); // ������һ����ɶ˿�

	if (NULL == m_hIOCompletionPort)
	{
		printf("������ɶ˿�ʧ�ܣ������룺%d\n",WSAGetLastError());
		return false;
	}

	m_nThreads = WORKER_THREADS_PER_PROCESSOR * _GetNoOfProcessors(); // ���ݱ����еĴ�����������������Ӧ���߳���

	m_phWorkerThreads = new HANDLE[m_nThreads]; // Ϊ�������̳߳�ʼ�����

	// ���ݼ�������������������߳�
	DWORD nThreadID;
	for (int i = 0; i < m_nThreads; ++i)
	{
		THREADPARAMS_WORKER* pThreadParams = new THREADPARAMS_WORKER;
		pThreadParams->pIOCPModel = this;
		pThreadParams->nThreadNo = i+1;
		m_phWorkerThreads[i] = ::CreateThread(0,0,_WorkerThread,(void*)pThreadParams,0,&nThreadID);
	}

	printf_s("����_WorkerThread %d��.\n",m_nThreads);

	return true;
}

// ��ʼ��socket
bool CIOCPModel::_InitializeListenSocket()
{
	// AcceptEx��GetAcceptExSockAddrs��Guid�����ڵ�������ָ��
	GUID guidAcceptEx = WSAID_ACCEPTEX;
	GUID guidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;

	sockaddr_in serverAddr; // ��������ַ��Ϣ

	m_pListenContext = new PER_SOCKET_CONTEXT; // �������ڼ�����socket����Ϣ

	// ��Ҫʹ���ص�IO�������ʹ��WSASocket������socket���ſ���֧���ص�IO����
	m_pListenContext->m_sock = WSASocket(AF_INET,SOCK_STREAM,0,NULL,0,WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == m_pListenContext->m_sock)
	{
		printf_s("��ʼ��socketʧ�ܣ������룺%d.\n",WSAGetLastError());
		return false;
	}
	else
	{
		printf_s("WSASocket���.\n");
	}

	// ��Listen Socket�󶨵���ɶ˿���
	if (NULL == CreateIoCompletionPort((HANDLE)m_pListenContext->m_sock,m_hIOCompletionPort,(DWORD)m_pListenContext,0))
	{
		printf_s("��Listen Socket����ɶ˿�ʧ��,������:%d\n",WSAGetLastError());
		return false;
	}
	else
	{
		printf_s("Listen Socket����ɶ˿����.\n");
	}

	// ����ַ��Ϣ
	ZeroMemory((char*)&serverAddr,sizeof(sockaddr_in));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY); // ���κο��õ�ip
	//serverAddr.sin_addr.s_addr = inet_addr(m_strIP.GetString()); // ��ָ����ip
	serverAddr.sin_port = htons(m_nPort);

	// �󶨵�ַ�Ͷ˿�
	if (SOCKET_ERROR == bind(m_pListenContext->m_sock,(sockaddr*)&serverAddr,sizeof(serverAddr)))
	{
		printf_s("bind()����ִ�д���.\n");
		return false;
	}
	else
	{
		printf_s("bind()���\n");
	}

	// ��ʼ����
	if (SOCKET_ERROR == listen(m_pListenContext->m_sock,SOMAXCONN))
	{
		printf_s("listen()����ִ�д���.\n");
		return false;
	}
	else
	{
		printf_s("listen()���\n");
	}

	// ʹ��AcceptEx��������Ϊ���������WinSock2�淶֮���΢�������ṩ����չ����
	// ������Ҫ�����ȡһ�º�����ָ�룬
	// ��ȡAcceptEx����ָ��
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
		printf_s("WSAIoctlδ�ܻ�ȡAcceptEx����ָ�룬�����룺%d.\n",WSAGetLastError());
		this->_DeInitialize();
		return false;
	}

	// ��ȡGetAcceptExSockAddrs����ָ�룬Ҳ��ͬ��
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
		printf_s("WSAIoctlδ�ܻ�ȡGetAcceptExSockAddrs����ָ�룬�����룺%d.\n",WSAGetLastError());
		this->_DeInitialize();
		return false;
	}

	// ΪAcceptEx׼��������Ȼ��Ͷ��AcceptEx I/O����
	for (int i = 0; i < MAX_POST_ACCEPT; ++i)
	{
		PER_IO_CONTEXT* pAcceptIoContext = m_pListenContext->GetNewIoContext();
		if (!this->_PostAccept(pAcceptIoContext))
		{
			m_pListenContext->RemoveContext(pAcceptIoContext);
			return false;
		}
	}

	printf_s("Ͷ��%d��AcceptEx�������.\n",MAX_POST_ACCEPT);

	return true;
}

// ����ͷŵ�������Դ
void CIOCPModel::_DeInitialize()
{
	DeleteCriticalSection(&m_csContextList); // ɾ���ͻ����б�Ļ�����

	RELEASE_HANDLE(m_hShutdownEvent); // �ر�ϵͳ�˳��¼����

	// �ͷŹ������߳̾��ָ��
	for (int i = 0; i < m_nThreads; ++i)
	{
		RELEASE_HANDLE(m_phWorkerThreads[i]);
	}

	RELEASE(m_phWorkerThreads);

	RELEASE_HANDLE(m_hIOCompletionPort); // �ر�IOCP���

	RELEASE(m_pListenContext); // �رռ���socket��context��Ϣ

	printf_s("�ͷ���Դ���.\n");
}

// Ͷ��Accept����
bool CIOCPModel::_PostAccept(PER_IO_CONTEXT* pAcceptIoContext)
{
	CHECK_BOOL_BOOL((INVALID_SOCKET != m_pListenContext->m_sock),"_PostAccept��Ч��socket\n");

	// ׼������
	DWORD dwBytes = 0;
	pAcceptIoContext->m_opType = ACCEPT_POSTED;
	WSABUF* p_wbuf = &pAcceptIoContext->m_wsaBuf;
	OVERLAPPED* p_ol = &pAcceptIoContext->m_overlapped;

	// Ϊ�Ժ�������Ŀͻ���׼����socket�������봫ͳAccept���������
	pAcceptIoContext->m_sock = WSASocket(AF_INET,SOCK_STREAM,IPPROTO_TCP,NULL,0,WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == pAcceptIoContext->m_sock)
	{
		printf_s("��������accept��socketʧ�ܣ������룺%d.\n",WSAGetLastError());
		return false;
	}

	// Ͷ��AcceptEx
	if (FALSE == m_lpfnAcceptEx(m_pListenContext->m_sock,pAcceptIoContext->m_sock,p_wbuf->buf,p_wbuf->len-((sizeof(SOCKADDR_IN)+16)*2),
		sizeof(SOCKADDR_IN)+16,sizeof(SOCKADDR_IN)+16,&dwBytes,p_ol))
	{
		if (WSA_IO_PENDING != WSAGetLastError())
		{
			printf_s("Ͷ��AcceptEx����ʧ�ܣ������룺%d.\n",WSAGetLastError());
			return false;
		}
	}

	return true;
}

// ����Accept���󣨿ͻ�������ʱ�����д���
bool CIOCPModel::_DoAccept(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext)
{
	SOCKADDR_IN* pCliendAddr = NULL;
	SOCKADDR_IN* pLocalAddr = NULL;
	int remoteLen = sizeof(SOCKADDR_IN), localLen = sizeof(SOCKADDR_IN);

	// ��ȡ�ͻ��ˡ����ص�ַ��Ϣ��ͬʱ��ȡ�ͻ��˷����ĵ�һ������
	this->m_lpfnGetAcceptExSockAddrs(pIoContext->m_wsaBuf.buf,pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN)+16)*2),
		sizeof(SOCKADDR_IN)+16,sizeof(SOCKADDR_IN)+16,(LPSOCKADDR*)&pLocalAddr,&localLen,(LPSOCKADDR*)&pCliendAddr,&remoteLen);

	printf_s("�ͻ���%s:%d����.\n",inet_ntoa(pCliendAddr->sin_addr),ntohs(pCliendAddr->sin_port));
	printf_s("�ͻ���%s:%d��Ϣ:%s.\n",inet_ntoa(pCliendAddr->sin_addr),ntohs(pCliendAddr->sin_port),pIoContext->m_wsaBuf.buf);

	// pSocketContext��listenSocket�ϵ�context������Ҫ������һ�����ӣ�������Ҫ�½�һ��socketContext��Ӧ�������socket
	PER_SOCKET_CONTEXT* pNewSocketContext = new PER_SOCKET_CONTEXT;
	pNewSocketContext->m_sock = pIoContext->m_sock;
	memcpy(&(pNewSocketContext->m_clientAddr),pCliendAddr,sizeof(SOCKADDR_IN));

	// �������socket����ɶ˿ڰ�
	if (!this->_AssociateWithIOCP(pNewSocketContext))
	{
		RELEASE(pNewSocketContext);
		return false;
	}

	// ����socketContext�µ�ioContext���������socket��Ͷ�ݵĵ�һ��Recv����
	PER_IO_CONTEXT* pNewIoContext = pNewSocketContext->GetNewIoContext();
	pNewIoContext->m_opType = RECV_POSTED;
	pNewIoContext->m_sock = pNewSocketContext->m_sock;
	//memcpy(pNewIoContext->m_szBuffer,pIoContext->m_szBuffer,MAX_BUFFER_LEN); // ���Buffer��Ҫ�������͸��Ƴ���

	// ����Ϻ󣬾Ϳ��ԾͿ��������socket��Ͷ��������
	if (!this->_PostRecv(pNewIoContext))
	{
		pNewSocketContext->RemoveContext(pNewIoContext);
		return false;
	}

	// ���Ͷ�ݳɹ����Ͱ������Ч�ͻ�����Ϣ��ӵ�contextList��ͳһ����
	this->_AddToContextList(pNewSocketContext);

	// ��listenSocket��ioContext���ã�׼��Ͷ���µ�acceptEx
	pIoContext->ResetBuffer();
	return this->_PostAccept(pIoContext);
}

// Ͷ�ݽ�����������(��ʼ���ã������������Ĵ���ͻ�������)
bool CIOCPModel::_PostRecv(PER_IO_CONTEXT* pIoContext)
{
	// ��ʼ������
	DWORD dwFlags = 0;
	DWORD dwBytes = 0;
	WSABUF* p_wbuf = &pIoContext->m_wsaBuf;
	OVERLAPPED* p_ol = &pIoContext->m_overlapped;

	pIoContext->ResetBuffer();
	pIoContext->m_opType = RECV_POSTED;

	// ��ʼ����Ϻ�Ͷ��WSARecv����
	int nBytesRecv = WSARecv(pIoContext->m_sock,p_wbuf,1,&dwBytes,&dwFlags,p_ol,NULL);
	if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError())) // ����ֵ���󣬲��Ҵ����벻��pending�Ļ�����˵���ص�����ʧ����
	{
		printf_s("Ͷ�ݵ�һ��WSARecvʧ��.\n");
		return false;
	}
	return true;
}

// �������ݵ���ʱ�����д���
bool CIOCPModel::_DoRecv(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext)
{
	// ȡ����
	SOCKADDR_IN* pClientAddr = &pSocketContext->m_clientAddr;
	printf_s("�յ�%s:%d��Ϣ:%s\n",inet_ntoa(pClientAddr->sin_addr),ntohs(pClientAddr->sin_port),pIoContext->m_wsaBuf.buf);

	// Ȼ��Ͷ����һ��WSARecv����
	return _PostRecv(pIoContext);
}

// �����(socket)�󶨵���ɶ˿���
bool CIOCPModel::_AssociateWithIOCP(PER_SOCKET_CONTEXT* pContext)
{
	HANDLE hTemp = CreateIoCompletionPort((HANDLE)pContext->m_sock,m_hIOCompletionPort,(DWORD)pContext,0);

	if (hTemp == NULL)
	{
		printf_s("ִ��CreateIoCompletionPort()���ִ��󣬴�����:%d.",GetLastError());
		return false;
	}

	return true;
}

// ���ͻ��������Ϣ�洢��������
void CIOCPModel::_AddToContextList(PER_SOCKET_CONTEXT* pSocketContext)
{
	EnterCriticalSection(&m_csContextList);

	m_arrayClientContext.push_back(pSocketContext);

	LeaveCriticalSection(&m_csContextList);
}

// �Ƴ�ĳ���ض���Context
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

// ��տͻ�����Ϣ
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

// ��ȡ����IP��ַ
string CIOCPModel::GetLocalIP()
{
	// ��ñ���������
	char hostname[MAX_PATH] = {0};
	gethostname(hostname,MAX_PATH);
	hostent FAR* lpHostEnt = gethostbyname(hostname);
	if (lpHostEnt == NULL)
	{
		return DEFAULT_IP;
	}

	//  ȡ��IP��ַ�б��еĵ�һ�����ص�IP����Ϊһ̨�������ܻ�󶨶��IP��
	LPSTR lpAddr = lpHostEnt->h_addr_list[0];

	// ��IP��ַת�����ַ�����ʽ
	in_addr inAddr;
	memmove(&inAddr,lpAddr,4);
	m_strIP = inet_ntoa(inAddr);
	return m_strIP;
}

// ��ȡ�����еĴ�����������
int CIOCPModel::_GetNoOfProcessors()
{
	SYSTEM_INFO si;

	GetSystemInfo(&si);

	return si.dwNumberOfProcessors;
}

// �жϿͻ���Socket�Ƿ��Ѿ��Ͽ���������һ����Ч��Socket��Ͷ��WSARecv����������쳣
// ʹ�õķ����ǳ��������socket�������ݣ��ж����socket���õķ���ֵ
// ��Ϊ����ͻ��������쳣�Ͽ�(����ͻ��˱������߰ε����ߵ�)��ʱ�򣬷����������޷��յ��ͻ��˶Ͽ���֪ͨ��
bool CIOCPModel::_IsSocketAlive(SOCKET s)
{
	int nByteSend = send(s,"",0,0);
	if (-1 == nByteSend) return false;
	return true;
}

// ��ʾ��������ɶ˿ڵĴ���
bool CIOCPModel::_HandleError(PER_SOCKET_CONTEXT* pContext, const DWORD& dwError)
{
	// ����ǳ�ʱ���ͼ����ȴ�
	if (WAIT_TIMEOUT == dwError)
	{
		// ȷ�Ͽͻ����Ƿ񻹻���
		if (!_IsSocketAlive(pContext->m_sock))
		{
			printf_s("��⵽�ͻ����쳣�˳�.");
			this->_RemoveContext(pContext);
			return true;
		}
		else
		{
			printf_s("���������ʱ�������С���");
			return true;
		}
	}

	// �����ǿͻ����쳣�˳���
	else if (ERROR_NETNAME_DELETED == dwError)
	{
		printf_s("��⵽�ͻ����쳣�˳�.");
		return true;
	}
	else
	{
		printf_s("��ɶ˿ڲ������ִ����߳��˳���������:%d.\n",dwError);
		return false;
	}
}
