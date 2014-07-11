#pragma once

#include <vector>
#include <string>

#include <WinSock2.h>
#include <MSWSock.h>
#pragma comment(lib,"ws2_32.lib")

#define MAX_BUFFER_LEN 8192 // ���������� (1024*8)
#define DEFAULT_PORT 12345 // Ĭ�϶˿�
#define DEFAULT_IP "127.0.0.1" // Ĭ��ip��ַ

#define CHECK_POINTER(x,msg) {if(x == NULL){printf_s(msg); return;}} // У��ָ��
#define CHECK_POINTER_BOOL(x,msg) {if(x == NULL){printf_s(msg); return false;}} // У��ָ��
#define CHECK_BOOL_BOOL(x,msg) {if(!x){printf_s(msg); return false;}} // У��ָ��

using std::vector;
using std::string;

// ����ɶ˿���Ͷ�ݵ�I/O��������
enum OPERATION_TYPE
{
	ACCEPT_POSTED, // ��־Ͷ�ݵ�accept����
	SEND_POSTED, // ��־Ͷ�ݵ��Ƿ��Ͳ���
	RECV_POSTED, // ��־Ͷ�ݵ��ǽ��ܲ���
	NULL_POSTED, // ���ڳ�ʼ��,������
};

// ��IO���ݽṹ�嶨��
struct PER_IO_CONTEXT
{
	OVERLAPPED m_overlapped; // ÿһ���ص�����������ص��ṹ�����ÿһ��socket��ÿһ����������Ҫ��һ���� �������Ա��������Ҫ�����ڽṹ��ĵ�һλ����Ϊ��CONTAINING_RECORDҪ���������ҵ�PER_IO_CONTEXT��ָ���ַ
	SOCKET m_sock; // ������������ʹ�õ�socket
	WSABUF m_wsaBuf; // WSA���͵Ļ����������ڸ��ص�������������
	char m_szBuffer[MAX_BUFFER_LEN]; // �����WSABUF��ľ����ַ��Ļ�����
	OPERATION_TYPE m_opType; // ��ʶ������������ͣ���Ӧ�����ö�٣�

	// ��ʼ��
	PER_IO_CONTEXT()
	{
		ZeroMemory(&m_overlapped,sizeof(m_overlapped));
		ZeroMemory(m_szBuffer,MAX_BUFFER_LEN);
		m_sock = INVALID_SOCKET;
		m_wsaBuf.buf = m_szBuffer;
		m_wsaBuf.len = MAX_BUFFER_LEN;
		m_opType = NULL_POSTED;
	}

	// �ͷ�
	~PER_IO_CONTEXT()
	{
		if (m_sock != INVALID_SOCKET)
		{
			closesocket(m_sock);
			m_sock = INVALID_SOCKET;
		}
	}

	// ���û���������
	void ResetBuffer()
	{
		ZeroMemory(m_szBuffer,MAX_BUFFER_LEN);
	}
};

// ��������ݽṹ�嶨�壨����ÿһ����ɶ˿ڣ�Ҳ����ÿһ��socket�Ĳ�����
struct PER_SOCKET_CONTEXT
{
	SOCKET m_sock; // ÿһ���ͻ������ӵ�socket
	SOCKADDR_IN m_clientAddr; // �ͻ��˵ĵ�ַ
	vector<PER_IO_CONTEXT*> m_arrayIoContext; // �ͻ�������������������ݣ�����ÿһ���ͻ���socket���ǿ���������ͬʱͶ�ݶ��IO����ģ�

	PER_SOCKET_CONTEXT()
	{
		m_sock = INVALID_SOCKET;
		memset(&m_clientAddr,0,sizeof(m_clientAddr));
	}

	~PER_SOCKET_CONTEXT()
	{
		if (m_sock != INVALID_SOCKET)
		{
			closesocket(m_sock);
			m_sock = INVALID_SOCKET;
		}
		// �ͷŵ����е�IO����������
		for (size_t i = 0; i < m_arrayIoContext.size(); ++i)
		{
			delete m_arrayIoContext.at(i);
		}
		m_arrayIoContext.clear();
	}

	// ��ȡһ���µ�IoContext
	PER_IO_CONTEXT* GetNewIoContext()
	{
		PER_IO_CONTEXT* p = new PER_IO_CONTEXT;
		m_arrayIoContext.push_back(p);
		return p;
	}

	// ���������Ƴ�һ��ָ����IoContext
	void RemoveContext(PER_IO_CONTEXT* pContext)
	{
		CHECK_POINTER(pContext,"RemoveContext��ָ��\n");
		for (vector<PER_IO_CONTEXT*>::iterator it = m_arrayIoContext.begin(); it != m_arrayIoContext.end(); ++it)
		{
			if (pContext == *it)
			{
				delete pContext;
				pContext = NULL;
				m_arrayIoContext.erase(it);
				break;
			}
		}
	}
};

// �������̲߳���
class CIOCPModel;
struct THREADPARAMS_WORKER
{
	CIOCPModel* pIOCPModel; // ��ָ�룬���ڵ������еĺ���
	int nThreadNo; // �̱߳��
};

// CIOCPModel����
class CIOCPModel
{
public:
	CIOCPModel();
	~CIOCPModel();

public:
	bool Start(); // ����������
	void Stop(); // ֹͣ������
	bool LoadSocketLib(); // ����socket��
	void UnloadSocketLib() { WSACleanup(); } // ж��socket�⣬��������
	string GetLocalIP(); // ��ñ���IP��ַ
	void SetPort(const int& port) { m_nPort = port; } // ���ü����˿�

protected:
	bool _InitializeIOCP(); // ��ʼ��IOCP
	bool _InitializeListenSocket(); // ��ʼ��socket
	void _DeInitialize(); // ����ͷ���Դ
	bool _PostAccept(PER_IO_CONTEXT* pAcceptIoContext); // Ͷ��Accept����
	bool _PostRecv(PER_IO_CONTEXT* pIoContext); // Ͷ�ݽ�����������
	bool _DoAccept(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext); // �пͻ��������ʱ����д���
	bool _DoRecv(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext); // �������ݵ����ʱ����д���
	void _AddToContextList(PER_SOCKET_CONTEXT* pSocketContext); // ���ͻ��˵������Ϣ�洢��������
	void _RemoveContext(PER_SOCKET_CONTEXT* pSocketContext); // ���ͻ��˵���Ϣ���������Ƴ�
	void _ClearContextList(); // ��տͻ�����Ϣ
	bool _AssociateWithIOCP(PER_SOCKET_CONTEXT* pContext); // ������󶨵���ɶ˿���
	bool _HandleError(PER_SOCKET_CONTEXT* pContext, const DWORD& dwError); // ������ɶ˿��ϵĴ���
	static DWORD WINAPI _WorkerThread(LPVOID lpParam); // �̺߳�����ΪIOCP�������Ĺ������߳�
	int _GetNoOfProcessors(); // ��ñ����Ĵ���������
	bool _IsSocketAlive(SOCKET s); // �жϿͻ���socket�Ƿ��Ѿ��Ͽ�

private:
	HANDLE m_hShutdownEvent; // ����֪ͨ�߳��˳����¼���Ϊ�˸��õ��˳��߳�
	HANDLE m_hIOCompletionPort; // ��ɶ˿ھ��
	HANDLE* m_phWorkerThreads; // �������̵߳ľ��ָ��
	int m_nThreads; // ���ɵ��߳�����
	string m_strIP; // �������˵�IP��ַ
	int m_nPort; // �������˵ļ����˿�
	CRITICAL_SECTION m_csContextList; // ����worker�߳�ͬ���Ļ�����
	vector<PER_SOCKET_CONTEXT*> m_arrayClientContext; // �ͻ���socket��context��Ϣ
	PER_SOCKET_CONTEXT* m_pListenContext; // ���ڼ�����socket��context��Ϣ
	LPFN_ACCEPTEX m_lpfnAcceptEx; // AcceptEx�ĺ���ָ��
	LPFN_GETACCEPTEXSOCKADDRS m_lpfnGetAcceptExSockAddrs; // GetAcceptExSockAddrs�ĺ���ָ��
};
