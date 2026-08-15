/*
 * Copyright: JessMA Open Source (ldcsaa@gmail.com)
 *
 * Author	: Bruce Liang
 * Website	: https://github.com/ldcsaa
 * Project	: https://github.com/ldcsaa/HP-Socket
 * Blog		: http://www.cnblogs.com/ldcsaa
 * Wiki		: http://www.oschina.net/p/hp-socket
 * QQ Group	: 44636872, 75375912
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/******************************************************************************
Module:  HPSocket

Usage:
		方法一：
		--------------------------------------------------------------------------------------
		0. 应用程序包含 HPTypeDef.h / SocketInterface.h / HPSocket.h 头文件
		1. 调用 HP_Create_Xxx() 函数创建 HPSocket 对象
		2. 使用完毕后调用 HP_Destroy_Xxx() 函数销毁 HPSocket 对象

		方法二：
		--------------------------------------------------------------------------------------
		0. 应用程序包含 SocketInterface.h 和 HPSocket.h 头文件
		1. 创建 CXxxPtr 智能指针，通过智能指针使用 HPSocket 对象

Release:
		<-- 动态链接库 -->
		1. x86/HPSocket.dll				- (32位/MBCS/Release)
		2. x86/HPSocket_D.dll			- (32位/MBCS/DeBug)
		3. x86/HPSocket_U.dll			- (32位/UNICODE/Release)
		4. x86/HPSocket_UD.dll			- (32位/UNICODE/DeBug)
		5. x64/HPSocket.dll				- (64位/MBCS/Release)
		6. x64/HPSocket_D.dll			- (64位/MBCS/DeBug)
		7. x64/HPSocket_U.dll			- (64位/UNICODE/Release)
		8. x64/HPSocket_UD.dll			- (64位/UNICODE/DeBug)

		<-- 静态链接库 -->
		!!注意!!：使用 HPSocket 静态库时，需要在工程属性中定义预处理宏 -> HPSOCKET_STATIC_LIB
		1. x86/static/HPSocket.lib		- (32位/MBCS/Release)
		2. x86/static/HPSocket_D.lib	- (32位/MBCS/DeBug)
		3. x86/static/HPSocket_U.lib	- (32位/UNICODE/Release)
		4. x86/static/HPSocket_UD.lib	- (32位/UNICODE/DeBug)
		5. x64/static/HPSocket.lib		- (64位/MBCS/Release)
		6. x64/static/HPSocket_D.lib	- (64位/MBCS/DeBug)
		7. x64/static/HPSocket_U.lib	- (64位/UNICODE/Release)
		8. x64/static/HPSocket_UD.lib	- (64位/UNICODE/DeBug)

******************************************************************************/

#pragma once

#include "SocketInterface.h"

/*****************************************************************************************************************************************************/
/****************************************************************** TCP/UDP Exports ******************************************************************/
/*****************************************************************************************************************************************************/

/**************************************************/
/************** HPSocket 对象智能指针 **************/

template<class T, class _Listener, class _Creator> class CHPObjectPtr
{
public:
	CHPObjectPtr& Reset(ITcpPackClient* pObj = nullptr)
	{
		if(pObj != m_pObj)
		{
			if(m_pObj)
				_Creator::Destroy(m_pObj);

			m_pObj = pObj;
		}

		return *this;
	}

	CHPObjectPtr& Attach(ITcpPackClient* pObj)
	{
		return Reset(pObj);
	}

	ITcpPackClient* Detach()
	{
		ITcpPackClient* pObj	= m_pObj;
		m_pObj	= nullptr;

		return pObj;
	}

	BOOL IsValid	()	const	{return m_pObj != nullptr	;}
	ITcpPackClient* Get			()	const	{return m_pObj				;}
	ITcpPackClient* operator ->	()	const	{return m_pObj				;}
	operator ITcpPackClient*		()	const	{return m_pObj				;}

	CHPObjectPtr& operator = (ITcpPackClient* pObj)	{return Reset(pObj)	;}

public:
	CHPObjectPtr(_Listener* pListener = nullptr)
	{
		m_pObj = _Creator::Create(pListener);
	}

	CHPObjectPtr(BOOL bCreate, _Listener* pListener = nullptr)
	{
		m_pObj = bCreate ? _Creator::Create(pListener) : nullptr;
	}

	virtual ~CHPObjectPtr()
	{
		Reset();
	}

private:
	CHPObjectPtr(const CHPObjectPtr&);
	CHPObjectPtr& operator = (const CHPObjectPtr&);

protected:
	ITcpPackClient* m_pObj;
};

/**************************************************/
/**************** HPSocket 导出函数 ****************/

// 创建 ITcpServer 对象
HPSOCKET_API ITcpServer* HP_Create_TcpServer(ITcpServerListener* pListener);
// 创建 ITcpAgent 对象
HPSOCKET_API ITcpAgent* HP_Create_TcpAgent(ITcpAgentListener* pListener);
// 创建 ITcpClient 对象
HPSOCKET_API ITcpClient* HP_Create_TcpClient(ITcpClientListener* pListener);

// 创建 ITcpPackServer 对象
HPSOCKET_API ITcpPackServer* HP_Create_TcpPackServer(ITcpServerListener* pListener);
// 创建 ITcpPackAgent 对象
HPSOCKET_API ITcpPackAgent* HP_Create_TcpPackAgent(ITcpAgentListener* pListener);
// 创建 ITcpPackClient 对象
HPSOCKET_API ITcpPackClient* HP_Create_TcpPackClient(ITcpClientListener* pListener);

// 销毁 ITcpServer 对象
HPSOCKET_API void HP_Destroy_TcpServer(ITcpServer* pServer);
// 销毁 ITcpAgent 对象
HPSOCKET_API void HP_Destroy_TcpAgent(ITcpAgent* pAgent);
// 销毁 ITcpClient 对象
HPSOCKET_API void HP_Destroy_TcpClient(ITcpClient* pClient);

// 销毁 ITcpPackServer 对象
HPSOCKET_API void HP_Destroy_TcpPackServer(ITcpPackServer* pServer);
// 销毁 ITcpPackAgent 对象
HPSOCKET_API void HP_Destroy_TcpPackAgent(ITcpPackAgent* pAgent);
// 销毁 ITcpPackClient 对象
HPSOCKET_API void HP_Destroy_TcpPackClient(ITcpPackClient* pClient);

#ifdef _UDP_SUPPORT

// 创建 IUdpServer 对象
HPSOCKET_API IUdpServer* HP_Create_UdpServer(IUdpServerListener* pListener);
// 创建 IUdpClient 对象
HPSOCKET_API IUdpClient* HP_Create_UdpClient(IUdpClientListener* pListener);
// 创建 IUdpCast 对象
HPSOCKET_API IUdpCast* HP_Create_UdpCast(IUdpCastListener* pListener);
// 创建 IUdpNode 对象
HPSOCKET_API IUdpNode* HP_Create_UdpNode(IUdpNodeListener* pListener);
// 创建 IUdpArqServer 对象
HPSOCKET_API IUdpArqServer* HP_Create_UdpArqServer(IUdpServerListener* pListener);
// 创建 IUdpArqClient 对象
HPSOCKET_API IUdpArqClient* HP_Create_UdpArqClient(IUdpClientListener* pListener);

// 销毁 IUdpServer 对象
HPSOCKET_API void HP_Destroy_UdpServer(IUdpServer* pServer);
// 销毁 IUdpClient 对象
HPSOCKET_API void HP_Destroy_UdpClient(IUdpClient* pClient);
// 销毁 IUdpCast 对象
HPSOCKET_API void HP_Destroy_UdpCast(IUdpCast* pCast);
// 销毁 IUdpNode 对象
HPSOCKET_API void HP_Destroy_UdpNode(IUdpNode* pNode);
// 销毁 IUdpArqServer 对象
HPSOCKET_API void HP_Destroy_UdpArqServer(IUdpArqServer* pServer);
// 销毁 IUdpArqClient 对象
HPSOCKET_API void HP_Destroy_UdpArqClient(IUdpArqClient* pClient);

#endif

// ITcpServer 对象创建器
struct TcpServer_Creator
{
	static ITcpServer* Create(ITcpServerListener* pListener)
	{
		return HP_Create_TcpServer(pListener);
	}

	static void Destroy(ITcpServer* pServer)
	{
		HP_Destroy_TcpServer(pServer);
	}
};

// ITcpAgent 对象创建器
struct TcpAgent_Creator
{
	static ITcpAgent* Create(ITcpAgentListener* pListener)
	{
		return HP_Create_TcpAgent(pListener);
	}

	static void Destroy(ITcpAgent* pAgent)
	{
		HP_Destroy_TcpAgent(pAgent);
	}
};

// ITcpClient 对象创建器
struct TcpClient_Creator
{
	static ITcpClient* Create(ITcpClientListener* pListener)
	{
		return HP_Create_TcpClient(pListener);
	}

	static void Destroy(ITcpClient* pClient)
	{
		HP_Destroy_TcpClient(pClient);
	}
};



// ITcpPackServer 对象创建器
struct TcpPackServer_Creator
{
	static ITcpPackServer* Create(ITcpServerListener* pListener)
	{
		return HP_Create_TcpPackServer(pListener);
	}

	static void Destroy(ITcpPackServer* pServer)
	{
		HP_Destroy_TcpPackServer(pServer);
	}
};

// ITcpPackAgent 对象创建器
struct TcpPackAgent_Creator
{
	static ITcpPackAgent* Create(ITcpAgentListener* pListener)
	{
		return HP_Create_TcpPackAgent(pListener);
	}

	static void Destroy(ITcpPackAgent* pAgent)
	{
		HP_Destroy_TcpPackAgent(pAgent);
	}
};

// ITcpPackClient 对象创建器
struct TcpPackClient_Creator
{
	static ITcpPackClient* Create(ITcpClientListener* pListener)
	{
		return HP_Create_TcpPackClient(pListener);
	}

	static void Destroy(ITcpPackClient* pClient)
	{
		HP_Destroy_TcpPackClient(pClient);
	}
};

// ITcpServer 对象智能指针
typedef CHPObjectPtr<ITcpServer, ITcpServerListener, TcpServer_Creator>			CTcpServerPtr;
// ITcpAgent 对象智能指针
typedef CHPObjectPtr<ITcpAgent, ITcpAgentListener, TcpAgent_Creator>			CTcpAgentPtr;
// ITcpClient 对象智能指针
typedef CHPObjectPtr<ITcpClient, ITcpClientListener, TcpClient_Creator>			CTcpClientPtr;
// ITcpPackServer 对象智能指针
typedef CHPObjectPtr<ITcpPackServer, ITcpServerListener, TcpPackServer_Creator>	CTcpPackServerPtr;
// ITcpPackAgent 对象智能指针
typedef CHPObjectPtr<ITcpPackAgent, ITcpAgentListener, TcpPackAgent_Creator>	CTcpPackAgentPtr;
// ITcpPackClient 对象智能指针
typedef CHPObjectPtr<ITcpPackClient, ITcpClientListener, TcpPackClient_Creator>	CTcpPackClientPtr;

#ifdef _UDP_SUPPORT

// IUdpServer 对象创建器
struct UdpServer_Creator
{
	static IUdpServer* Create(IUdpServerListener* pListener)
	{
		return HP_Create_UdpServer(pListener);
	}

	static void Destroy(IUdpServer* pServer)
	{
		HP_Destroy_UdpServer(pServer);
	}
};

// IUdpClient 对象创建器
struct UdpClient_Creator
{
	static IUdpClient* Create(IUdpClientListener* pListener)
	{
		return HP_Create_UdpClient(pListener);
	}

	static void Destroy(IUdpClient* pClient)
	{
		HP_Destroy_UdpClient(pClient);
	}
};

// IUdpCast 对象创建器
struct UdpCast_Creator
{
	static IUdpCast* Create(IUdpCastListener* pListener)
	{
		return HP_Create_UdpCast(pListener);
	}

	static void Destroy(IUdpCast* pCast)
	{
		HP_Destroy_UdpCast(pCast);
	}
};

// IUdpNode 对象创建器
struct UdpNode_Creator
{
	static IUdpNode* Create(IUdpNodeListener* pListener)
	{
		return HP_Create_UdpNode(pListener);
	}

	static void Destroy(IUdpNode* pNode)
	{
		HP_Destroy_UdpNode(pNode);
	}
};

// IUdpArqServer 对象创建器
struct UdpArqServer_Creator
{
	static IUdpArqServer* Create(IUdpServerListener* pListener)
	{
		return HP_Create_UdpArqServer(pListener);
	}

	static void Destroy(IUdpArqServer* pServer)
	{
		HP_Destroy_UdpArqServer(pServer);
	}
};

// IUdpArqClient 对象创建器
struct UdpArqClient_Creator
{
	static IUdpArqClient* Create(IUdpClientListener* pListener)
	{
		return HP_Create_UdpArqClient(pListener);
	}

	static void Destroy(IUdpArqClient* pClient)
	{
		HP_Destroy_UdpArqClient(pClient);
	}
};

// IUdpServer 对象智能指针
typedef CHPObjectPtr<IUdpServer, IUdpServerListener, UdpServer_Creator>			CUdpServerPtr;
// IUdpClient 对象智能指针
typedef CHPObjectPtr<IUdpClient, IUdpClientListener, UdpClient_Creator>			CUdpClientPtr;
// IUdpCast 对象智能指针
typedef CHPObjectPtr<IUdpCast, IUdpCastListener, UdpCast_Creator>				CUdpCastPtr;
// IUdpNode 对象智能指针
typedef CHPObjectPtr<IUdpNode, IUdpNodeListener, UdpNode_Creator>				CUdpNodePtr;
// IUdpArqServer 对象智能指针
typedef CHPObjectPtr<IUdpArqServer, IUdpServerListener, UdpArqServer_Creator>	CUdpArqServerPtr;
// IUdpArqClient 对象智能指针
typedef CHPObjectPtr<IUdpArqClient, IUdpClientListener, UdpArqClient_Creator>	CUdpArqClientPtr;

#endif

/*****************************************************************************************************************************************************/
/*************************************************************** Global Function Exports *************************************************************/
/*****************************************************************************************************************************************************/

// 获取 HPSocket 版本号（4 个字节分别为：主版本号，子版本号，修正版本号，构建编号）
HPSOCKET_API DWORD HP_GetHPSocketVersion();

// 获取错误描述文本
HPSOCKET_API LPCTSTR HP_GetSocketErrorDesc(EnSocketError enCode);
// 调用系统的 GetLastError() 方法获取系统错误代码
HPSOCKET_API DWORD SYS_GetLastError	();
// 调用系统的 WSAGetLastError() 方法获取系统错误代码
HPSOCKET_API int SYS_WSAGetLastError();
// 调用系统的 setsockopt()
HPSOCKET_API int SYS_SetSocketOption(SOCKET sock, int level, int name, LPVOID val, int len);
// 调用系统的 getsockopt()
HPSOCKET_API int SYS_GetSocketOption(SOCKET sock, int level, int name, LPVOID val, int* len);
// 调用系统的 ioctlsocket()
HPSOCKET_API int SYS_IoctlSocket(SOCKET sock, long cmd, ULONG* arg);
// 调用系统的 WSAIoctl()
HPSOCKET_API int SYS_WSAIoctl(SOCKET sock, DWORD dwIoControlCode, LPVOID lpvInBuffer, DWORD cbInBuffer, LPVOID lpvOutBuffer, DWORD cbOutBuffer, LPDWORD lpcbBytesReturned);

// 设置 socket 选项：IPPROTO_TCP -> TCP_NODELAY
HPSOCKET_API int SYS_SSO_NoDelay(SOCKET sock, BOOL bNoDelay);
// 设置 socket 选项：SOL_SOCKET -> SO_DONTLINGER
HPSOCKET_API int SYS_SSO_DontLinger(SOCKET sock, BOOL bDont);
// 设置 socket 选项：SOL_SOCKET -> SO_LINGER
HPSOCKET_API int SYS_SSO_Linger(SOCKET sock, USHORT l_onoff, USHORT l_linger);
// 设置 socket 选项：SOL_SOCKET -> SO_RCVBUF
HPSOCKET_API int SYS_SSO_RecvBuffSize(SOCKET sock, int size);
// 设置 socket 选项：SOL_SOCKET -> SO_SNDBUF
HPSOCKET_API int SYS_SSO_SendBuffSize(SOCKET sock, int size);
// 设置 socket 选项：SOL_SOCKET -> SO_RCVTIMEO
HPSOCKET_API int SYS_SSO_RecvTimeOut(SOCKET sock, int ms);
// 设置 socket 选项：SOL_SOCKET -> SO_SNDTIMEO
HPSOCKET_API int SYS_SSO_SendTimeOut(SOCKET sock, int ms);
// 设置 socket 选项：SOL_SOCKET -> SO_EXCLUSIVEADDRUSE / SO_REUSEADDR
HPSOCKET_API int SYS_SSO_ReuseAddress(SOCKET sock, EnReuseAddressPolicy opt);
// 设置 socket 选项：SOL_SOCKET -> SO_EXCLUSIVEADDRUSE
HPSOCKET_API int SYS_SSO_ExclusiveAddressUse(SOCKET sock, BOOL bExclusive);

// 获取 SOCKET 本地地址信息
HPSOCKET_API BOOL SYS_GetSocketLocalAddress(SOCKET socket, TCHAR lpszAddress[], int& iAddressLen, USHORT& usPort);
// 获取 SOCKET 远程地址信息
HPSOCKET_API BOOL SYS_GetSocketRemoteAddress(SOCKET socket, TCHAR lpszAddress[], int& iAddressLen, USHORT& usPort);

/* 枚举主机 IP 地址 */
HPSOCKET_API BOOL SYS_EnumHostIPAddresses(LPCTSTR lpszHost, EnIPAddrType enType, LPTIPAddr** lpppIPAddr, int& iIPAddrCount);
/* 释放 LPTIPAddr* */
HPSOCKET_API BOOL SYS_FreeHostIPAddresses(LPTIPAddr* lppIPAddr);
/* 检查字符串是否符合 IP 地址格式 */
HPSOCKET_API BOOL SYS_IsIPAddress(LPCTSTR lpszAddress, EnIPAddrType* penType = nullptr);
/* 通过主机名获取 IP 地址 */
HPSOCKET_API BOOL SYS_GetIPAddress(LPCTSTR lpszHost, TCHAR lpszIP[], int& iIPLenth, EnIPAddrType& enType);

/* 64 位网络字节序转主机字节序 */
HPSOCKET_API ULONGLONG SYS_NToH64(ULONGLONG value);
/* 64 位主机字节序转网络字节序 */
HPSOCKET_API ULONGLONG SYS_HToN64(ULONGLONG value);
/* 短整型高低字节交换 */
HPSOCKET_API USHORT SYS_SwapEndian16(USHORT value);
/* 长整型高低字节交换 */
HPSOCKET_API DWORD SYS_SwapEndian32(DWORD value);
/* 检查是否小端字节序 */
HPSOCKET_API BOOL SYS_IsLittleEndian();

/* 分配内存 */
HPSOCKET_API LPBYTE SYS_Malloc(int size);
/* 重新分配内存 */
HPSOCKET_API LPBYTE SYS_Realloc(LPBYTE p, int size);
/* 释放内存 */
HPSOCKET_API VOID SYS_Free(LPBYTE p);
/* 分配内存块 */
HPSOCKET_API LPVOID SYS_Calloc(int number, int size);

// CP_XXX -> UNICODE
HPSOCKET_API BOOL SYS_CodePageToUnicodeEx(int iCodePage, const char szSrc[], int iSrcLength, WCHAR szDest[], int& iDestLength);
// UNICODE -> CP_XXX
HPSOCKET_API BOOL SYS_UnicodeToCodePageEx(int iCodePage, const WCHAR szSrc[], int iSrcLength, char szDest[], int& iDestLength);
// GBK -> UNICODE
HPSOCKET_API BOOL SYS_GbkToUnicodeEx(const char szSrc[], int iSrcLength, WCHAR szDest[], int& iDestLength);
// UNICODE -> GBK
HPSOCKET_API BOOL SYS_UnicodeToGbkEx(const WCHAR szSrc[], int iSrcLength, char szDest[], int& iDestLength);
// UTF8 -> UNICODE
HPSOCKET_API BOOL SYS_Utf8ToUnicodeEx(const char szSrc[], int iSrcLength, WCHAR szDest[], int& iDestLength);
// UNICODE -> UTF8
HPSOCKET_API BOOL SYS_UnicodeToUtf8Ex(const WCHAR szSrc[], int iSrcLength, char szDest[], int& iDestLength);
// GBK -> UTF8
HPSOCKET_API BOOL SYS_GbkToUtf8Ex(const char szSrc[], int iSrcLength, char szDest[], int& iDestLength);
// UTF8 -> GBK
HPSOCKET_API BOOL SYS_Utf8ToGbkEx(const char szSrc[], int iSrcLength, char szDest[], int& iDestLength);

// CP_XXX -> UNICODE
HPSOCKET_API BOOL SYS_CodePageToUnicode(int iCodePage, const char szSrc[], WCHAR szDest[], int& iDestLength);
// UNICODE -> CP_XXX
HPSOCKET_API BOOL SYS_UnicodeToCodePage(int iCodePage, const WCHAR szSrc[], char szDest[], int& iDestLength);
// GBK -> UNICODE
HPSOCKET_API BOOL SYS_GbkToUnicode(const char szSrc[], WCHAR szDest[], int& iDestLength);
// UNICODE -> GBK
HPSOCKET_API BOOL SYS_UnicodeToGbk(const WCHAR szSrc[], char szDest[], int& iDestLength);
// UTF8 -> UNICODE
HPSOCKET_API BOOL SYS_Utf8ToUnicode(const char szSrc[], WCHAR szDest[], int& iDestLength);
// UNICODE -> UTF8
HPSOCKET_API BOOL SYS_UnicodeToUtf8(const WCHAR szSrc[], char szDest[], int& iDestLength);
// GBK -> UTF8
HPSOCKET_API BOOL SYS_GbkToUtf8(const char szSrc[], char szDest[], int& iDestLength);
// UTF8 -> GBK
HPSOCKET_API BOOL SYS_Utf8ToGbk(const char szSrc[], char szDest[], int& iDestLength);

// 计算 Base64 编码后长度
HPSOCKET_API DWORD SYS_GuessBase64EncodeBound(DWORD dwSrcLen);
// 计算 Base64 解码后长度
HPSOCKET_API DWORD SYS_GuessBase64DecodeBound(const BYTE* lpszSrc, DWORD dwSrcLen);
// Base64 编码（返回值：0 -> 成功，-3 -> 输入数据不正确，-5 -> 输出缓冲区不足）
HPSOCKET_API int SYS_Base64Encode(const BYTE* lpszSrc, DWORD dwSrcLen, BYTE* lpszDest, DWORD& dwDestLen);
// Base64 解码（返回值：0 -> 成功，-3 -> 输入数据不正确，-5 -> 输出缓冲区不足）
HPSOCKET_API int SYS_Base64Decode(const BYTE* lpszSrc, DWORD dwSrcLen, BYTE* lpszDest, DWORD& dwDestLen);

// 计算 URL 编码后长度
HPSOCKET_API DWORD SYS_GuessUrlEncodeBound(const BYTE* lpszSrc, DWORD dwSrcLen);
// 计算 URL 解码后长度
HPSOCKET_API DWORD SYS_GuessUrlDecodeBound(const BYTE* lpszSrc, DWORD dwSrcLen);
// URL 编码（返回值：0 -> 成功，-3 -> 输入数据不正确，-5 -> 输出缓冲区不足）
HPSOCKET_API int SYS_UrlEncode(BYTE* lpszSrc, DWORD dwSrcLen, BYTE* lpszDest, DWORD& dwDestLen);
// URL 解码（返回值：0 -> 成功，-3 -> 输入数据不正确，-5 -> 输出缓冲区不足）
HPSOCKET_API int SYS_UrlDecode(BYTE* lpszSrc, DWORD dwSrcLen, BYTE* lpszDest, DWORD& dwDestLen);



// 创建 IHPThreadPool 对象
HPSOCKET_API IHPThreadPool* HP_Create_ThreadPool(IHPThreadPoolListener* pListener = nullptr);
// 销毁 IHPThreadPool 对象
HPSOCKET_API void HP_Destroy_ThreadPool(IHPThreadPool* pThreadPool);

/*
* 名称：创建 TSocketTask 对象
* 描述：创建任务对象，该对象最终需由 HP_Destroy_SocketTaskObj() 销毁
*		
* 参数：		fnTaskProc	-- 任务处理函数
*			pSender		-- 发起对象
*			dwConnID	-- 连接 ID
*			pBuffer		-- 数据缓冲区
*			iBuffLen	-- 数据缓冲区长度
*			enBuffType	-- 数据缓冲区类型（默认：TBT_COPY）
*							TBT_COPY	：（深拷贝）pBuffer 复制到 TSocketTask 对象。此后 TSocketTask 对象与 pBuffer 不再有任何关联
*											-> 适用于 pBuffer 不大或 pBuffer 生命周期不受控的场景
*							TBT_REFER	：（浅拷贝）pBuffer 不复制到 TSocketTask 对象，需确保 TSocketTask 对象生命周期内 pBuffer 必须有效
*											-> 适用于 pBuffer 较大或 pBuffer 可重用，并且 pBuffer 生命周期受控的场景
*							TBT_ATTACH	：（附属）执行浅拷贝，但 TSocketTask 对象会获得 pBuffer 的所有权，并负责释放 pBuffer，避免多次缓冲区拷贝
*											-> 注意：pBuffer 必须由 SYS_Malloc()/SYS_Calloc() 函数分配才能使用本类型，否则可能会发生内存访问错误
*			wParam		-- 自定义参数
*			lParam		-- 自定义参数
* 返回值：	LPTSocketTask
*/
HPSOCKET_API LPTSocketTask HP_Create_SocketTaskObj(Fn_SocketTaskProc fnTaskProc, PVOID pSender, CONNID dwConnID, LPCBYTE pBuffer, INT iBuffLen, EnTaskBufferType enBuffType = TBT_COPY, WPARAM wParam = 0, LPARAM lParam = 0);

// 销毁 TSocketTask 对象
HPSOCKET_API void HP_Destroy_SocketTaskObj(LPTSocketTask pTask);


/*****************************************************************************************************************************************************/
/********************************************************* Compressor / Decompressor Exports *********************************************************/
/*****************************************************************************************************************************************************/



