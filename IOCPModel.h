#pragma once

#include <vector>
#include <string>

#include <WinSock2.h>
#include <MSWSock.h>
#pragma comment(lib,"ws2_32.lib")

#define MAX_BUFFER_LEN 8192 // 缓冲区长度 (1024*8)
#define DEFAULT_PORT 12345 // 默认端口
#define DEFAULT_IP "127.0.0.1" // 默认ip地址

#define CHECK_POINTER(x,msg) {if(x == NULL){printf_s(msg); return;}} // 校验指针
#define CHECK_POINTER_BOOL(x,msg) {if(x == NULL){printf_s(msg); return false;}} // 校验指针
#define CHECK_BOOL_BOOL(x,msg) {if(!x){printf_s(msg); return false;}} // 校验指针

using std::vector;
using std::string;

// 在完成端口上投递的I/O操作类型
enum OPERATION_TYPE
{
	ACCEPT_POSTED, // 标志投递的accept操作
	SEND_POSTED, // 标志投递的是发送操作
	RECV_POSTED, // 标志投递的是接受操作
	NULL_POSTED, // 用于初始化,无意义
};

// 单IO数据结构体定义
struct PER_IO_CONTEXT
{
	OVERLAPPED m_overlapped; // 每一个重叠网络操作的重叠结构（针对每一个socket的每一个操作，都要有一个） 另：这个成员变量必须要定义在结构体的第一位，因为宏CONTAINING_RECORD要根据他来找到PER_IO_CONTEXT的指针地址
	SOCKET m_sock; // 这个网络操作所使用的socket
	WSABUF m_wsaBuf; // WSA类型的缓冲区，用于给重叠操作传参数的
	char m_szBuffer[MAX_BUFFER_LEN]; // 这个是WSABUF里的具体字符的缓冲区
	OPERATION_TYPE m_opType; // 标识网络操作的类型（对应上面的枚举）

	// 初始化
	PER_IO_CONTEXT()
	{
		ZeroMemory(&m_overlapped,sizeof(m_overlapped));
		ZeroMemory(m_szBuffer,MAX_BUFFER_LEN);
		m_sock = INVALID_SOCKET;
		m_wsaBuf.buf = m_szBuffer;
		m_wsaBuf.len = MAX_BUFFER_LEN;
		m_opType = NULL_POSTED;
	}

	// 释放
	~PER_IO_CONTEXT()
	{
		if (m_sock != INVALID_SOCKET)
		{
			closesocket(m_sock);
			m_sock = INVALID_SOCKET;
		}
	}

	// 重置缓冲区内容
	void ResetBuffer()
	{
		ZeroMemory(m_szBuffer,MAX_BUFFER_LEN);
	}
};

// 单句柄数据结构体定义（用于每一个完成端口，也就是每一个socket的参数）
struct PER_SOCKET_CONTEXT
{
	SOCKET m_sock; // 每一个客户端连接的socket
	SOCKADDR_IN m_clientAddr; // 客户端的地址
	vector<PER_IO_CONTEXT*> m_arrayIoContext; // 客户端网络操作上下文数据（对于每一个客户端socket，是可以在上面同时投递多个IO请求的）

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
		// 释放掉所有的IO上下文数据
		for (size_t i = 0; i < m_arrayIoContext.size(); ++i)
		{
			delete m_arrayIoContext.at(i);
		}
		m_arrayIoContext.clear();
	}

	// 获取一个新的IoContext
	PER_IO_CONTEXT* GetNewIoContext()
	{
		PER_IO_CONTEXT* p = new PER_IO_CONTEXT;
		m_arrayIoContext.push_back(p);
		return p;
	}

	// 从数组中移除一个指定的IoContext
	void RemoveContext(PER_IO_CONTEXT* pContext)
	{
		CHECK_POINTER(pContext,"RemoveContext空指针\n");
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

// 工作者线程参数
class CIOCPModel;
struct THREADPARAMS_WORKER
{
	CIOCPModel* pIOCPModel; // 类指针，用于调用类中的函数
	int nThreadNo; // 线程编号
};

// CIOCPModel定义
class CIOCPModel
{
public:
	CIOCPModel();
	~CIOCPModel();

public:
	bool Start(); // 启动服务器
	void Stop(); // 停止服务器
	bool LoadSocketLib(); // 加载socket库
	void UnloadSocketLib() { WSACleanup(); } // 卸载socket库，彻底完事
	string GetLocalIP(); // 获得本机IP地址
	void SetPort(const int& port) { m_nPort = port; } // 设置监听端口

protected:
	bool _InitializeIOCP(); // 初始化IOCP
	bool _InitializeListenSocket(); // 初始化socket
	void _DeInitialize(); // 最后释放资源
	bool _PostAccept(PER_IO_CONTEXT* pAcceptIoContext); // 投递Accept请求
	bool _PostRecv(PER_IO_CONTEXT* pIoContext); // 投递接收数据请求
	bool _DoAccept(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext); // 有客户端连入的时候进行处理
	bool _DoRecv(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext); // 在有数据到达的时候进行处理
	void _AddToContextList(PER_SOCKET_CONTEXT* pSocketContext); // 将客户端的相关信息存储到数组中
	void _RemoveContext(PER_SOCKET_CONTEXT* pSocketContext); // 将客户端的信息从数组中移除
	void _ClearContextList(); // 清空客户端信息
	bool _AssociateWithIOCP(PER_SOCKET_CONTEXT* pContext); // 将句柄绑定到完成端口中
	bool _HandleError(PER_SOCKET_CONTEXT* pContext, const DWORD& dwError); // 处理完成端口上的错误
	static DWORD WINAPI _WorkerThread(LPVOID lpParam); // 线程函数，为IOCP请求服务的工作者线程
	int _GetNoOfProcessors(); // 获得本机的处理器数量
	bool _IsSocketAlive(SOCKET s); // 判断客户端socket是否已经断开

private:
	HANDLE m_hShutdownEvent; // 用来通知线程退出的事件，为了更好的退出线程
	HANDLE m_hIOCompletionPort; // 完成端口句柄
	HANDLE* m_phWorkerThreads; // 工作者线程的句柄指针
	int m_nThreads; // 生成的线程数量
	string m_strIP; // 服务器端的IP地址
	int m_nPort; // 服务器端的监听端口
	CRITICAL_SECTION m_csContextList; // 用于worker线程同步的互斥量
	vector<PER_SOCKET_CONTEXT*> m_arrayClientContext; // 客户端socket的context信息
	PER_SOCKET_CONTEXT* m_pListenContext; // 用于监听的socket的context信息
	LPFN_ACCEPTEX m_lpfnAcceptEx; // AcceptEx的函数指针
	LPFN_GETACCEPTEXSOCKADDRS m_lpfnGetAcceptExSockAddrs; // GetAcceptExSockAddrs的函数指针
};
