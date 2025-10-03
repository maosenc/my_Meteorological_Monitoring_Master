在该目录下开发数据接收和发送的客户端以及云服务器服务端，并且将接收数据传输给qt
编译命令：
一般执行make all 因为server程序在云服务器中编译最好，不要执行make all
编译发送端客户端:
	make send
	这是Ubuntu中运行的发送端程序，将数据发送到云服务器
编译接收客户端：
	make recv
	这是开发板中运行的接收端程序，接收云服务器发送来的数据
Qt程序仅备份，不参与该目录下的编译，实际qt程序见目录Meteorological_Monitoring_Master

2025/9/21  cl

