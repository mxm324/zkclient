#pragma once

#ifdef ZKCLIENT_EXPORTS
#    define ZKCLIENT_API __declspec(dllexport)
#else
#  if (defined(__CYGWIN__) || defined(WIN32)) && !defined(USE_STATIC_LIB)
#    define ZKCLIENT_API __declspec(dllimport)
#  else
#    define ZKCLIENT_API
#  endif
#endif

/*
	资源池节点分布
					|Source				资源节点(Mcu000001/Mcu000002)
Resource -- Mcu --	|ApplyQueue			申请列表
					|ReserveQueue		预占列表
	1、使用之前，确保服务器端已经创建了永久节点 Resource、Source、ApplyQueue、ReserveQueue
	2、客户端均为异步回调模型，函数返回值只代表操作参数无误
	3、connect有自动重连机制
	4、资源申请流程（加入申请队列->排到第一个->选择资源（连接资源）->设置预占值->退出申请队列->延时退出预占队列（auto_delete_time) )
	5、长时间的调试会导致zk与服务器断链，但会自动连上，但之前的异步操作会失败
*/
namespace ZkClient
{
	class ZKCLIENT_API NodeValue{
	public:
		static NodeValue* Create();
		static void Destory( NodeValue* value );
	public:
		unsigned GetCount() const;
		const char* GetKey( unsigned index );
		const char* GetValue( unsigned index );
		const char* GetValue( const char* key );	
		void AddValue( const char* key, const char* value );
		void DeleteAll();
		bool DeleteValue( const char* key );
	public:
		void DeSerialize( const char* buff, int len );
		bool Serialize( char* buff, int& len );
		NodeValue* Clone();
	private:
		NodeValue();
		~NodeValue();
		class NodeValueImpl;
		NodeValueImpl* impl_;
	};
	
	#define INVALID_ID -1
	typedef int NodeID;
	typedef enum EmZkSystemState{ zkConnected, zkDisconnect, zkConnecting, zkReConnecting }ZkSystemState;

	// 回调类型
	/*
		ConnectCb			建链成功的回调
		ReConectCb			重连成功的回调
		ReConnectingCb		正在重连的回调（也说明zk服务器异常）
	*/
	typedef enum EmZkCallbackType{ ConnectCb, ReConnectCb, ReConnectingCb, DisconnectCb, SourceChangeCb, ApplyInited, 
		ApplyFailCb, ApplySuccessCb, RegisterCb, ChangeCb, DeleteCb, ApplyAckCb }ZkCallbackType;
	
	typedef struct TCallbackParam 
	{
		ZkCallbackType type;
		// 操作结果
		int result;
		// 上下文
		void* context;			

		// 所有NodeValue均为client内部分配资源，不要存储
		// 若要存储，请调用NodeValue::Create 然后通过序列化和反序列化完成
		struct SourceChangeParam 
		{
			// 资源列表
			NodeValue** values;	
			int len;
		};
		struct RegisterParam
		{
			NodeID id;
		};
		struct ApplySuccessParam
		{
			// 资源列表 [in]
			NodeValue** source_values;
			int source_len;
			
			// 预占列表 [in]
			NodeValue** reserve_values;
			int reserve_len;

			// 预占信息（使用者需要填写） [out]
			NodeValue* reserve_value;
			// 自动删除预占时间（使用者需要填写 0表示直到断链才删除） [out]
			unsigned auto_delete_time;
			// 已经填写预占队列（使用者需要填写） [out]
			bool has_choosed;
		};
		struct ApplyAckParam{
			// 完整路径
			const char* full_path;
			// 索引，客户端唯一
			int index;
		};
		union{
			// 注册回调参数 当RegisterCB/ChangeCb/DeleteCb的时候使用
			RegisterParam register_param;
			// 资源变化回调参数 当SourceChangeCb的时候使用
			SourceChangeParam source_change_param;
			// 资源申请成功回调参数 当ApplySuccessCb的时候使用
			ApplySuccessParam apply_success_param;
			// Apply申请回复,索引可作为随机数使用
			ApplyAckParam apply_ack_param;
		};
	}CallbackParam;

	typedef void(*ZkCallback)( CallbackParam* param );

	class ZKCLIENT_API IZkRegisterClient
	{
	public:
		static IZkRegisterClient* Create( ZkCallback callback, void* context = 0,
			char* root_path = "/Resource", char* source_path = "/Source" );
		static void Destory( IZkRegisterClient* client );	
	public:
		/*
		连接函数 
		[in]	host="x.x.x.x:2181"	2181为默认端口
		*/
		bool Connect( const char* host, int time_out = 10000 );
		/*
		主动断链 会清除所有资源注册节点
		*/
		int DisConnect();
		/*
		注册资源 
		[in]	res="Mcu" or "Mpu"...  
		[in]	value为需要存放的节点信息（能力、连接方式等）
		[out]	id用于后续操作该资源（zk上注册资源，路径会随机生成）
		return 操作结果 ZOk为正常，错误参看ERRORS
		*/
		int Register( const char* res, NodeValue* value, NodeID& id );
		/*
		改变资源信息
		[in]	id 由Register返回的信息  
		[in]	value 需要修改的信息
		return 操作结果 ZOk为正常，错误参看ERRORS
		*/
		int Change( NodeID& id, NodeValue* value );
		/*
		主动删除资源
		[in]	id 由Register返回的信息  
		return 操作结果 ZOk为正常，错误参看ERRORS
		*/
		int Delete( NodeID& id );
		/*
		获取当前系统状态
		*/
		ZkSystemState GetSystemState();
	public:
		/*
			打印当前状态（默认IO输出）
		*/
		void Print();
		class ZkRegisterClientImpl;
	private:
		ZkRegisterClientImpl* impl_;
		IZkRegisterClient( ZkCallback callback, void* context = 0 ,
			char* root_path = "/Resource", char* source_path = "/Source" );
		~IZkRegisterClient(void);	
	};

	/*
		目前存在两种方式使用申请客户端
		1、只调用connect接口，然后在ZkCallback接口中处理SourceChangeCb，然后直接使用资源。
		2、调用connect，并且调用apply，使用排队方式申请，处理ZkCallback接口中的ApplySuccessCb,
			同时要负责填写输出参数
	*/
	class ZKCLIENT_API IZkApplyClient
	{
	public:
		static IZkApplyClient* Create( char* res_type,
			ZkCallback callback,
			void* context = 0,
			char* root_name = "/Resource", 
			char* apply_queue_name = "/ApplyQueue",
			char* reserve_queue_name = "/ReserveQueue",
			char* source_name = "/Source");
		static void Destory( IZkApplyClient* client );
	public:
		/*
		连接函数 
		[in]	host="x.x.x.x:2181"	2181为默认端口
		*/
		bool Connect( const char* host, int time_out = 10000 );
		int Disconnect();
		/*
		申请资源
		[in]	申请超时
		需要在ZkCallback=ApplyInited 后才能申请
		*/
		int Apply( int time_out = 10000 );
		/*
		获取当前系统状态
		*/
		ZkSystemState GetSystemState();	

	public:
		/*
			打印当前状态（默认IO输出）
		*/
		void Print();
		class ZkApplyClientImpl;	
	private:
		ZkApplyClientImpl* impl_;
		IZkApplyClient( char* res_type, 
			ZkCallback callback,
			void* context = 0,
			char* root_name = "/Resource", 
			char* apply_queue_name = "/ApplyQueue",
			char* reserve_queue_name = "/ReserveQueue",
			char* source_name = "/Source" );
		~IZkApplyClient(void);
	};
	
	typedef void (*PrintFunc)( const unsigned char id,const unsigned short lvl, const char* szUsage, ... );
	class ZKCLIENT_API IZkLogHelp{
	public:
		static void SetLog( bool is_open, int level = 1 );
		static void SetSystemLog( bool is_open, int level = 1 );
		static void SetPrintFunc(PrintFunc func);
	};
};
// 以下为zookeeper定义的错误号，注释掉，避免冲突
// EmZkErrors {
//   ZOK = 0, /*!< Everything is OK */
// 
//   /** System and server-side errors.
//    * This is never thrown by the server, it shouldn't be used other than
//    * to indicate a range. Specifically error codes greater than this
//    * value, but lesser than {@link #ZAPIERROR}, are system errors. */
//   ZSYSTEMERROR = -1,
//   ZRUNTIMEINCONSISTENCY = -2, /*!< A runtime inconsistency was found */
//   ZDATAINCONSISTENCY = -3, /*!< A data inconsistency was found */
//   ZCONNECTIONLOSS = -4, /*!< Connection to the server has been lost */
//   ZMARSHALLINGERROR = -5, /*!< Error while marshalling or unmarshalling data */
//   ZUNIMPLEMENTED = -6, /*!< Operation is unimplemented */
//   ZOPERATIONTIMEOUT = -7, /*!< Operation timeout */
//   ZBADARGUMENTS = -8, /*!< Invalid arguments */
//   ZINVALIDSTATE = -9, /*!< Invliad zhandle state */
//   ZNEWCONFIGNOQUORUM = -13, /*!< No quorum of new config is connected and
//                                  up-to-date with the leader of last commmitted
//                                  config - try invoking reconfiguration after new
//                                  servers are connected and synced */
//   ZRECONFIGINPROGRESS = -14, /*!< Reconfiguration requested while another
//                                   reconfiguration is currently in progress. This
//                                   is currently not supported. Please retry. */
// 
//   /** API errors.
//    * This is never thrown by the server, it shouldn't be used other than
//    * to indicate a range. Specifically error codes greater than this
//    * value are API errors (while values less than this indicate a
//    * {@link #ZSYSTEMERROR}).
//    */
//   ZAPIERROR = -100,
//   ZNONODE = -101, /*!< Node does not exist */
//   ZNOAUTH = -102, /*!< Not authenticated */
//   ZBADVERSION = -103, /*!< Version conflict */
//   ZNOCHILDRENFOREPHEMERALS = -108, /*!< Ephemeral nodes may not have children */
//   ZNODEEXISTS = -110, /*!< The node already exists */
//   ZNOTEMPTY = -111, /*!< The node has children */
//   ZSESSIONEXPIRED = -112, /*!< The session has been expired by the server */
//   ZINVALIDCALLBACK = -113, /*!< Invalid callback specified */
//   ZINVALIDACL = -114, /*!< Invalid ACL specified */
//   ZAUTHFAILED = -115, /*!< Client authentication failed */
//   ZCLOSING = -116, /*!< ZooKeeper is closing */
//   ZNOTHING = -117, /*!< (not error) no server responses to process */
//   ZSESSIONMOVED = -118, /*!<session moved to another server, so operation is ignored */
//   ZNOTREADONLY = -119, /*!< state-changing request is passed to read-only server */
//   ZEPHEMERALONLOCALSESSION = -120, /*!< Attempt to create ephemeral node on a local session */
//   ZNOWATCHER = -121, /*!< The watcher couldn't be found */
//   ZRWSERVERFOUND = -122 /*!< r/w server found while in r/o mode */
// };

