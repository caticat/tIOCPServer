#include <iostream>
#include "IOCPModel.h"

CIOCPModel IOCPModel;

int main()
{
	printf_s("server start\n");

	// 加载Winsock2.2库
	if (!IOCPModel.LoadSocketLib())
	{
		printf_s("加载Winsock2.2失败，服务器无法运行！");
		return 1;
	}

	// 开启服务
	if (!IOCPModel.Start())
	{
		printf_s("服务器启动失败！");
		return 2;
	}

	// 主线程停止循环
	string cmd = "";
	while (true)
	{
		printf_s(">");
		std::cin>>cmd;
		if ((cmd == "quit") || (cmd == "exit")) // 退出程序
		{
			IOCPModel.Stop(); // 关闭IOCP监听服务
			printf_s("exit server\n");
			break;
		}
	}

	printf_s("server end\n");
	return 0;
}
