#include <iostream>
#include "IOCPModel.h"

CIOCPModel IOCPModel;

int main()
{
	printf_s("server start\n");

	// ����Winsock2.2��
	if (!IOCPModel.LoadSocketLib())
	{
		printf_s("����Winsock2.2ʧ�ܣ��������޷����У�");
		return 1;
	}

	// ��������
	if (!IOCPModel.Start())
	{
		printf_s("����������ʧ�ܣ�");
		return 2;
	}

	// ���߳�ֹͣѭ��
	string cmd = "";
	while (true)
	{
		printf_s(">");
		std::cin>>cmd;
		if ((cmd == "quit") || (cmd == "exit")) // �˳�����
		{
			IOCPModel.Stop(); // �ر�IOCP��������
			printf_s("exit server\n");
			break;
		}
	}

	printf_s("server end\n");
	return 0;
}
