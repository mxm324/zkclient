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
	��Դ�ؽڵ�ֲ�
					|Source				��Դ�ڵ�(Mcu000001/Mcu000002)
Resource -- Mcu --	|ApplyQueue			�����б�
					|ReserveQueue		Ԥռ�б�
	1��ʹ��֮ǰ��ȷ�����������Ѿ����������ýڵ� Resource��Source��ApplyQueue��ReserveQueue
	2���ͻ��˾�Ϊ�첽�ص�ģ�ͣ���������ֵֻ���������������
	3��connect���Զ���������
	4����Դ�������̣������������->�ŵ���һ��->ѡ����Դ��������Դ��->����Ԥռֵ->�˳��������->��ʱ�˳�Ԥռ���У�auto_delete_time) )
	5����ʱ��ĵ��Իᵼ��zk������������������Զ����ϣ���֮ǰ���첽������ʧ��
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

	// �ص�����
	/*
		ConnectCb			�����ɹ��Ļص�
		ReConectCb			�����ɹ��Ļص�
		ReConnectingCb		���������Ļص���Ҳ˵��zk�������쳣��
	*/
	typedef enum EmZkCallbackType{ ConnectCb, ReConnectCb, ReConnectingCb, DisconnectCb, SourceChangeCb, ApplyInited, 
		ApplyFailCb, ApplySuccessCb, RegisterCb, ChangeCb, DeleteCb, ApplyAckCb }ZkCallbackType;
	
	typedef struct TCallbackParam 
	{
		ZkCallbackType type;
		// �������
		int result;
		// ������
		void* context;			

		// ����NodeValue��Ϊclient�ڲ�������Դ����Ҫ�洢
		// ��Ҫ�洢�������NodeValue::Create Ȼ��ͨ�����л��ͷ����л����
		struct SourceChangeParam 
		{
			// ��Դ�б�
			NodeValue** values;	
			int len;
		};
		struct RegisterParam
		{
			NodeID id;
		};
		struct ApplySuccessParam
		{
			// ��Դ�б� [in]
			NodeValue** source_values;
			int source_len;
			
			// Ԥռ�б� [in]
			NodeValue** reserve_values;
			int reserve_len;

			// Ԥռ��Ϣ��ʹ������Ҫ��д�� [out]
			NodeValue* reserve_value;
			// �Զ�ɾ��Ԥռʱ�䣨ʹ������Ҫ��д 0��ʾֱ��������ɾ���� [out]
			unsigned auto_delete_time;
			// �Ѿ���дԤռ���У�ʹ������Ҫ��д�� [out]
			bool has_choosed;
		};
		struct ApplyAckParam{
			// ����·��
			const char* full_path;
			// �������ͻ���Ψһ
			int index;
		};
		union{
			// ע��ص����� ��RegisterCB/ChangeCb/DeleteCb��ʱ��ʹ��
			RegisterParam register_param;
			// ��Դ�仯�ص����� ��SourceChangeCb��ʱ��ʹ��
			SourceChangeParam source_change_param;
			// ��Դ����ɹ��ص����� ��ApplySuccessCb��ʱ��ʹ��
			ApplySuccessParam apply_success_param;
			// Apply����ظ�,��������Ϊ�����ʹ��
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
		���Ӻ��� 
		[in]	host="x.x.x.x:2181"	2181ΪĬ�϶˿�
		*/
		bool Connect( const char* host, int time_out = 10000 );
		/*
		�������� �����������Դע��ڵ�
		*/
		int DisConnect();
		/*
		ע����Դ 
		[in]	res="Mcu" or "Mpu"...  
		[in]	valueΪ��Ҫ��ŵĽڵ���Ϣ�����������ӷ�ʽ�ȣ�
		[out]	id���ں�����������Դ��zk��ע����Դ��·����������ɣ�
		return ������� ZOkΪ����������ο�ERRORS
		*/
		int Register( const char* res, NodeValue* value, NodeID& id );
		/*
		�ı���Դ��Ϣ
		[in]	id ��Register���ص���Ϣ  
		[in]	value ��Ҫ�޸ĵ���Ϣ
		return ������� ZOkΪ����������ο�ERRORS
		*/
		int Change( NodeID& id, NodeValue* value );
		/*
		����ɾ����Դ
		[in]	id ��Register���ص���Ϣ  
		return ������� ZOkΪ����������ο�ERRORS
		*/
		int Delete( NodeID& id );
		/*
		��ȡ��ǰϵͳ״̬
		*/
		ZkSystemState GetSystemState();
	public:
		/*
			��ӡ��ǰ״̬��Ĭ��IO�����
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
		Ŀǰ�������ַ�ʽʹ������ͻ���
		1��ֻ����connect�ӿڣ�Ȼ����ZkCallback�ӿ��д���SourceChangeCb��Ȼ��ֱ��ʹ����Դ��
		2������connect�����ҵ���apply��ʹ���Ŷӷ�ʽ���룬����ZkCallback�ӿ��е�ApplySuccessCb,
			ͬʱҪ������д�������
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
		���Ӻ��� 
		[in]	host="x.x.x.x:2181"	2181ΪĬ�϶˿�
		*/
		bool Connect( const char* host, int time_out = 10000 );
		int Disconnect();
		/*
		������Դ
		[in]	���볬ʱ
		��Ҫ��ZkCallback=ApplyInited ���������
		*/
		int Apply( int time_out = 10000 );
		/*
		��ȡ��ǰϵͳ״̬
		*/
		ZkSystemState GetSystemState();	

	public:
		/*
			��ӡ��ǰ״̬��Ĭ��IO�����
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
// ����Ϊzookeeper����Ĵ���ţ�ע�͵��������ͻ
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

