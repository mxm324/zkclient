#include "IZkClient.h"
#include <map>
#include <string>
#include <vector>
#include <list>
#include <memory>
using namespace std;

#include <string.h>
#include "zookeeper.h"
#include "zookeeper_log.h"

#include "winport.h"

#include <stdarg.h> 
#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/*****************************************************************************
整体层次结构
	IObjectContainer				用于存储所有client对象（锁1）
		IZkRegisterClient			用户使用对象
		IZkApplyClient				
			ZkRegisterClientImpl	实际处理业务的对象（锁2）
			ZkApplyClientImpl
				Context				用于为zookeeper传递上下文（锁3）
				NodeValue			zk数据节点的抽象实现

锁模型
	锁1 用于解决client对象删除和回调可能引发的野指针问题
	锁2 用于解决用户调用client接口和回调调用client接口出现的多线程问题
	锁3 用于解决操作和回调使用上下文时出现的多线程问题
*****************************************************************************/

namespace ZkClient
{
#define DEL_PTR( p ) if(p) delete p; p = NULL;
#define DEL_PTR_ARRAY( p ) if(p) delete [] p; p = NULL;
#define MAX_BUFF	20480
bool is_print_open = false;
PrintFunc Print = NULL;
#define PRINT( print ) if ( is_print_open ) printf("[ZkClient] ");print;

// 暂用业务vcs的mid
#define		ZK_MID 102
#define		ZK_LOG_LVL_ERROR					1				// 程序运行错误(逻辑或业务出错),必须输出
#define		ZK_LOG_LVL_WARNING					2				// 告警信息, 可能正确, 也可能错误
#define		ZK_LOG_LVL_KEYSTATUS				3				// 程序运行到一个关键状态时的信息输出
#define		ZK_LOG_LVL_DETAIL					4				// 普通信息, 最好不要写进log文件中
#define		ZK_LOG_LVL_REPEAT					5				// 更高级别打印


void ZkClientPrint( unsigned short lvl, const char* content,...)
{
	char achBuf[1024] = { 0 };
	va_list argptr;		      
	va_start( argptr, content );
	vsnprintf(achBuf, 1024, content, argptr);
	if ( Print != NULL )
	{
		Print( lvl, ZK_MID, achBuf );
	}
	else
	{
		printf( achBuf );
	}
	va_end(argptr); 
}

	// 自动锁类
	class ZkAutoLock
	{
	public:
		ZkAutoLock( pthread_mutex_t* mutex )
		{
			mutex_ = mutex;
			pthread_mutex_lock( mutex_ );
		}
		~ZkAutoLock()
		{
			pthread_mutex_unlock( mutex_ );
		}

	private:
		pthread_mutex_t* mutex_;
	};

	// 自动初始化类
	class ZkAutoInit
	{
	public:
		ZkAutoInit( pthread_mutex_t* mutex ){	
			// 可重入锁（windows默认可重入）
			pthread_mutexattr_t attr;
			pthread_mutexattr_init( &attr );	
			pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

			int ret = pthread_mutex_init( mutex, &attr );
			mutex_ = mutex;

			if ( ret != 0 )
			{
				ZkClientPrint( ZK_LOG_LVL_ERROR, "create mutex fail error=%d\n", ret );
			}
		}
		~ZkAutoInit(){
			pthread_mutex_destroy(mutex_);
		}
	private:
		pthread_mutex_t* mutex_;
	};

	class NodeValue::NodeValueImpl
	{
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
		typedef std::map<std::string,std::string> Values;
		Values value_;
	};

	NodeValue* NodeValue::Create()
	{
		return new NodeValue();
	}
	void NodeValue::Destory( NodeValue* value )
	{
		DEL_PTR(value);
	}
	NodeValue::NodeValue()
	{
		impl_ = new NodeValueImpl();
	}
	NodeValue::~NodeValue()
	{
		DEL_PTR(impl_)
	}
	unsigned NodeValue::GetCount() const
	{
		return impl_->GetCount();
	}
	const char* NodeValue::GetKey( unsigned index )
	{
		return impl_->GetKey(index);
	}
	const char* NodeValue::GetValue( unsigned index )
	{
		return impl_->GetValue(index);
	}
	const char* NodeValue::GetValue( const char* key )
	{
		return impl_->GetValue(key);
	}
	void NodeValue::AddValue( const char* key, const char* value )
	{
		impl_->AddValue( key, value );
	}
	void NodeValue::DeleteAll()
	{
		impl_->DeleteAll();
	}
	bool NodeValue::DeleteValue( const char* key )
	{
		return impl_->DeleteValue(key);
	}
	void NodeValue::DeSerialize( const char* buff, int len )
	{
		impl_->DeSerialize( buff, len );
	}
	bool NodeValue::Serialize( char* buff, int& len )
	{
		return impl_->Serialize( buff, len );
	}
	NodeValue* NodeValue::Clone()
	{
		return impl_->Clone();
	}

	unsigned NodeValue::NodeValueImpl::GetCount() const
	{
		return value_.size();
	}

	const char* NodeValue::NodeValueImpl::GetKey( unsigned index )
	{
		if ( index >= value_.size() )
		{
			return NULL;
		}
		Values::iterator itr = value_.begin();
		unsigned temp = index;
		while ( temp > 0 )
		{
			itr++;
			temp--;
		}
		return itr->first.c_str();
	}

	const char* NodeValue::NodeValueImpl::GetValue( unsigned index )
	{
		if ( index >= value_.size() )
		{
			return NULL;
		}
		Values::iterator itr = value_.begin();
		unsigned temp = index;
		while ( temp > 0 )
		{
			itr++;
			temp--;
		}
		return itr->second.c_str();
	}

	const char* NodeValue::NodeValueImpl::GetValue( const char* key )
	{
		Values::iterator itr = value_.find( key );
		if ( itr != value_.end() )
		{
			return itr->second.c_str();
		}
		return NULL;
	}

	void NodeValue::NodeValueImpl::AddValue( const char* key, const char* value )
	{
		value_[key] = value;
	}

	void NodeValue::NodeValueImpl::DeleteAll()
	{
		value_.clear();
	}

	bool NodeValue::NodeValueImpl::DeleteValue( const char* key )
	{
		Values::iterator itr = value_.begin();
		while ( itr != value_.end() )
		{
			if ( itr->first == key )
			{
				value_.erase( itr );
				return true;
			}
			itr++;
		}
		return false;
	}

	void NodeValue::NodeValueImpl::DeSerialize( const char* buff, int len )
	{	
		if ( len != -1 )
		{
			int str_len = 0;
			while ( len - str_len > 0 )
			{
				string key = buff + str_len;
				str_len += key.length() + 1;

				string value = buff + str_len;
				str_len += value.length() + 1;

				value_[key] = value;
			}
		}
	}

	bool NodeValue::NodeValueImpl::Serialize( char* buff, int& len )
	{
		Values::iterator itr = value_.begin();
		unsigned buffer_len = 0;
		while ( itr != value_.end() )
		{	
			int str_len = itr->first.length() + 1;
			if ( buffer_len + str_len > len )
			{
				return false;
			}
			memcpy( buff + buffer_len, itr->first.c_str(), str_len );
			buffer_len += str_len;

			str_len = itr->second.length() + 1;
			if ( buffer_len + str_len > len )
			{
				return false;
			}
			memcpy( buff + buffer_len, itr->second.c_str(), str_len );
			buffer_len += str_len;
			itr++;
		}
		len = buffer_len;
		return true;
	}

	NodeValue* NodeValue::NodeValueImpl::Clone()
	{
		NodeValue* value = NodeValue::Create();
		Values::iterator itr = value_.begin();
		while ( itr != value_.end() )
		{
			value->AddValue( itr->first.c_str(), itr->second.c_str() );
			itr++;
		}
		return value;
	}

	typedef enum EmNodeState
	{
		emNormal = 0,
		emNodeCreating,		
	}NodeState;

	class NodeInfo 
	{
	public:
		NodeInfo():path_(""),node_state_(emNodeCreating),type_(""), node_need_update_(false){
			value_ = NodeValue::Create();
		}
		~NodeInfo(){
			NodeValue::Destory( value_ );
		}
		bool IsCreated(){ return (path_ != "");}
		const char* GetType(){ return type_.c_str(); }
		void SetType( const char* type ){ type_ = type; }
		const char* GetPath(){ return path_.c_str(); }
		void SetPath( const char* path ){ path_ = path; }
		NodeValue* GetValue(){ return value_; }
		void SetValue( NodeValue* value )
		{
			if ( value == value_ )
			{
				return;
			}
			value_->DeleteAll();
			for ( int i = 0; i < value->GetCount(); i++ )
			{
				value_->AddValue( value->GetKey(i), value->GetValue(i) );
			}
		}
		void SetNodeState( NodeState state  ){ node_state_ = state; }
		NodeState GetNodeState(){ return node_state_; }
		void NodeNeedUpdate( bool update ){ node_need_update_ = update; }
		bool IsNodeNeedUpdate(){ return node_need_update_; }
	protected:
		std::string path_;
		std::string type_;
		NodeValue* value_;
		NodeState node_state_;
		bool node_need_update_;
	};

	class Context;
	class IZkRegisterClient::ZkRegisterClientImpl
	{
	public:
		ZkRegisterClientImpl( ZkCallback callback, void* context = NULL,
			char* root_path = "/Resource", char* source_path = "/Source", IZkRegisterClient* parent =NULL)
			: zkhandle_(NULL), root_path_(root_path), source_path_(source_path), 
			system_state_(zkDisconnect), 
			callback_( callback ),
			callback_context_(context),index_(0),
			parent_(parent),connect_context_(NULL)
			{
				Init( true );
		}
		virtual ~ZkRegisterClientImpl(void){
			RemoveAllNode();
			DisConnect();
			Init( false );
		}
	public:
		bool Connect( const char* host, int time_out = 10000 );
		bool ReConnect( const char* host, int time_out = 10000 );
		bool RestoreRegisteredNode();
		int DisConnect();

		int Register( const char* res, NodeValue* value, NodeID& id );
		int Change( NodeID& id, NodeValue* value );
		int Delete( NodeID& id );
		ZkSystemState GetSystemState(){ return system_state_; }
	protected:
		NodeInfo* GetResInfo( NodeID id );	
		// 系统总Watch
		static void Watch(zhandle_t *zh, int type, int state, const char *path,void *watcherCtx);
		// Register回调
		static void StringCB(int rc, const char *value, const void *data);
		// Delete回调
		static void DeleteCB(int rc, const void *data);
		// Chanage回调
		static void StatCB(int rc, const struct Stat *stat, const void *data);
	

		void OnRegisterRsp( int rc, const char* value, NodeID id );
		void OnChangeRsp( int rc, const char* value, NodeID id );
		void OnDeleteRsp( int rc, const char* value, NodeID id );

		void UpdateNodeInfo( NodeID id, const char* path );
		void RemoveNodeInfo( NodeID id );
		void RemoveAllNodeInfo();
		
		void OnConnected();
		void OnConnecting();
		void OnDisconnected();
		void RemoveAllNode();
	public:
		IZkRegisterClient* GetParent(){ return parent_; }
		bool Init( bool bInit = true );
		void Print();
	private:
		pthread_mutex_t mutex_;
	private:
		// zk客户端句柄
		zhandle_t* zkhandle_;
		// 资源的路径
		string source_path_;
		// 根节点路径
		string root_path_;

		ZkCallback callback_;
		void* callback_context_;
		ZkSystemState system_state_;

		// 资源容器
		typedef map<NodeID,NodeInfo*> Nodes;
		Nodes nodes_;

		NodeID index_;
		IZkRegisterClient* parent_;

		// 连接用context
		Context* connect_context_;
	};

	class IZkApplyClient::ZkApplyClientImpl
	{
	public:
		ZkApplyClientImpl(	char* res_type, 
			ZkCallback callback,
			void* context = NULL,
			char* root_name = "/Resource", 
			char* apply_queue_name = "/ApplyQueue",
			char* reserve_queue_name = "/ReserveQueue",
			char* source_name = "/Source",
			IZkApplyClient* parent = NULL) 
			: callback_(callback),
			callback_context_(context),
			res_type_(res_type),zkhandle_(NULL), 
			apply_state_(idle), 
			system_state_(zkDisconnect),
			is_inited_(false),
			parent_(parent),
			connect_context_(NULL)
		{
			static int client_index = 0;
			client_id_ = ++client_index;
			source_path_ = root_name;
			source_path_ += "/";
			source_path_ += res_type_;

			reserve_queue_path_ = source_path_;
			apply_queue_path_ = source_path_;
			source_path_ += source_name;

			reserve_queue_path_ += reserve_queue_name;
			apply_queue_path_ += apply_queue_name;	

			reserve_list_watch_context_ = NULL;
			apply_list_watch_context_ = NULL;
			source_list_wath_context_ = NULL;

			reserve_node_watch_context_ = NULL;
			apply_node_watch_context_ = NULL;
			source_node_wath_context_ = NULL;

			connect_context_ = NULL;

			Init();
		}

		virtual ~ZkApplyClientImpl(void)
		{
			Disconnect();	
			Init( false );
		}
	public:
		bool Connect( const char* host, int time_out = 10000 );
		int Apply( unsigned time_out = 10000 );
		int Disconnect();
		ZkSystemState GetSystemState(){return system_state_;}
	protected:
		// 获取预占列表和资源列表
		bool LoadSource();
	protected:
		// 获取申请列表
		bool GetApplyList();
		// 更新申请列表（同时判定是否获得资源权限）
		bool UpdateApplyList( int rc, const struct String_vector *strings );
		// 是否已经到队列第一位置（资源权限）
		bool IsFirstPos( const char* path,  const struct String_vector *strings );
		// 触发用户回调，让其选择资源
		bool DoChoice();
		// 更新申请节点
		bool UpdateApplyNode( int rc, const char* path );

		// 获取资源列表
		int GetSourceList();
		// 获取资源节点信息
		int GetSourceNode( const char* path );
		// 更新资源列表
		int UpdateSourceList( int rc, const struct String_vector* strings );
		// 更新资源节点
		int UpdateSourceNode( int rc, const char *value, int value_len, const char* path );
		// 资源更新回调
		void NotifySourceList();

		// 获取预占列表
		int GetReserveList();
		// 获取预占节点
		int GetReserveNode( const char* path );
		// 更新预占列表
		int UpdateReserveList( int rc, const struct String_vector* strings );
		// 更新预占节点
		int UpdateReserveNode( int rc, const char *value, int value_len, const char* path );

		// 创建申请节点的回调
		static void ApplyNodeCB(int rc, const char *value, const void *data);
		// 申请列表回调
		static void ApplyListNotifyCB(int rc,const struct String_vector *strings, const void *data);
		// 申请列表改变Watch
		static void ApplyListChangeWatch(zhandle_t *zh, int type, int state, const char *path,void *watcherCtx);
		// 申请节点改变Watch
		static void PreNodeChangeWatch(zhandle_t *zh, int type, int state, const char *path,void *watcherCtx);

		// 列表回调（节点值变化不会引起回调）
		static void ListNotifyCB(int rc,const struct String_vector *strings, const void *data);
		// 节点回调
		static void NodeNotifyCB(int rc, const char *value, int value_len, const struct Stat *stat, const void *data);
		// 列表Watch
		static void ListChangeWatch(zhandle_t *zh, int type, int state, const char *path,void *watcherCtx);
		// 节点Watch
		static void NodeChangeWatch(zhandle_t *zh, int type, int state, const char *path,void *watcherCtx);

	private:
		// 对于watch来说，当不触发变化的时候，该上下文是不会自动删除的
		// 所以单独为其固定申请上下文，用于反复使用
		// 对于操作来说，操作过后必定会回调，所以可以及时回收
		// 删除对象的时候会整体回收为其分配的上下文
		Context* reserve_list_watch_context_;
		Context* apply_list_watch_context_;
		Context* source_list_wath_context_;

		Context* reserve_node_watch_context_;
		Context* apply_node_watch_context_;
		Context* source_node_wath_context_;

		// 连接用context
		Context* connect_context_;

	public:
		static void ReserveNodeCreateCB(int rc, const char *value, const void *data);
		static void VoidCB(int rc, const void *data){}
		static void StatCB(int rc, const struct Stat *stat, const void *data){}
		static void Watch(zhandle_t *zh, int type, int state, const char *path,void *watcherCtx);
		IZkApplyClient* GetParent(){ return parent_; }
	protected:		
		void OnDisconnected();
		void OnConnected();
		void OnConnecting();
	protected:
		void EndApply();

	protected:
		void RemoveReserveNode( const char* path );
		void RemoveSourceNode( const char* path );

	public:
		bool Init( bool bInit = true );
		void Print();
	private:
		ZkCallback callback_;
		void* callback_context_;
		zhandle_t* zkhandle_;
		string apply_queue_path_;
		string reserve_queue_path_;
		string source_path_;
		string res_type_;
		string apply_path_;

		typedef map<string,NodeValue*> ReserveQueue;
		ReserveQueue reserve_queue_;
		typedef map<string,NodeValue*> Sources;
		Sources sources_;

		enum EmApplyState{ idle,applying };

		bool is_inited_;
		EmApplyState apply_state_;
		ZkSystemState system_state_;

		int client_id_;
		IZkApplyClient* parent_;

		pthread_mutex_t mutex_;
	};

	typedef enum emNodeType{
		ApplyNode,
		ReserveNode,
		SourceNode
	}NodeType;

	//	上下文 
	// 上下文的使用，向回调传入ID，回调回来取上下文
	// 对于需要频繁使用上下文的，设置为成员变量，循环使用
	// 对于不频繁的，回调回来后自行删除（回调不成功也需要删除）
	class Context{
	public:
		static Context* Create( IZkRegisterClient::ZkRegisterClientImpl* register_client = 0, string path = "",  NodeID id = INVALID_ID )
		{
			ZkAutoLock lock( &Context::context_mutex_ );
			Context* context = new Context();
			context->context_id_ = ++Context::context_idx_;
			context->register_client_ = register_client;
			context->path_ = path;
			context->node_id_ = id;

			contexts_[context->context_id_] = context;
			return context;
		}

		static Context* Create( zhandle_t* zkhanlde = 0, IZkApplyClient::ZkApplyClientImpl* apply_client = 0, unsigned auto_delete_time = 0,string path = "", NodeType type = SourceNode)
		{
			ZkAutoLock lock( &Context::context_mutex_ );
			Context* context = new Context();
			context->context_id_ = ++Context::context_idx_;
			context->zkhanlde_ = zkhanlde;
			context->apply_client_ = apply_client;
			context->auto_delete_time_ = auto_delete_time;
			context->path_ = path;
			context->node_type_ = type;

			contexts_[context->context_id_] = context;

			return context;
		}

		static Context* Create( Context* src_context )
		{
			if ( src_context == NULL )
			{
				return NULL;
			}
			ZkAutoLock lock( &Context::context_mutex_ );
			Context* context = new Context();
			context->context_id_ = ++Context::context_idx_;			
			context->zkhanlde_ = src_context->zkhanlde_;
			context->apply_client_ = src_context->apply_client_;
			context->auto_delete_time_ = src_context->auto_delete_time_;
			context->path_ = src_context->path_;
			context->node_type_ = src_context->node_type_;
			context->register_client_ = src_context->register_client_;
			context->node_id_ = src_context->node_id_;

			contexts_[context->context_id_] = context;

			return context;
		}

		static void Destory( Context* context )
		{
			if ( context == NULL )
			{
				return;
			}
			ZkAutoLock lock( &Context::context_mutex_ );
			Contexts::iterator itr = contexts_.find( context->context_id_ );
			if ( itr != contexts_.end() )
			{
				DEL_PTR( itr->second );
				contexts_.erase( itr );
			}
		}

		static void Destory( int index )
		{
			ZkAutoLock lock( &Context::context_mutex_ );
			Contexts::iterator itr = contexts_.find( index );
			if ( itr != contexts_.end() )
			{
				DEL_PTR( itr->second );
				contexts_.erase( itr );
			}
		}
		static void Clear( IZkApplyClient* apply_client )
		{
			if ( apply_client == NULL )
			{
				return;
			}
			ZkAutoLock lock( &Context::context_mutex_ );
			Contexts::iterator itr = contexts_.begin();
			while ( itr != contexts_.end() )
			{
				Context* context = itr->second;
				IZkApplyClient::ZkApplyClientImpl* client = context->apply_client_;
				if ( client != NULL )
				{
					if ( client->GetParent() == apply_client )
					{
						DEL_PTR( itr->second );
						contexts_.erase( itr++ );
					}	
					else
					{
						itr++;
					}
				}
				else
				{
					itr++;
				}
			}
		}

		static void Clear( IZkRegisterClient* register_client )
		{
			if ( register_client == NULL )
			{
				return;
			}
			ZkAutoLock lock( &Context::context_mutex_ );
			Contexts::iterator itr = contexts_.begin();
			while ( itr != contexts_.end() )
			{
				Context* context = itr->second;
				IZkRegisterClient::ZkRegisterClientImpl* client = context->register_client_;
				if ( client != NULL )
				{
					if ( client->GetParent() == register_client )
					{
						DEL_PTR( itr->second );
						contexts_.erase( itr++ );
					}		
					else
					{
						itr++;
					}
				}
				else
				{
					itr++;
				}
			}
		}

		static bool GetContext( unsigned int index, Context& context )
		{
			ZkAutoLock lock( &Context::context_mutex_ );
			Contexts::iterator itr = contexts_.find( index );
			if ( itr != contexts_.end() )
			{
				context = *itr->second;
				return true;
			}
			return false;
		}

		static void FormatContext( Context* context )
		{
			if ( context == NULL )
			{
				return;
			}
			context->apply_client_ = 0;
			context->register_client_ = 0;
			context->node_type_ = SourceNode;
			context->auto_delete_time_ = 0;
			context->path_ = "";
			context->zkhanlde_ = 0;
			context->is_idle_ = true;
		}

		static void Print()
		{
			ZkAutoLock lock( &Context::context_mutex_ );
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "Context size = %d index = %d\n", contexts_.size(), context_idx_ );
		}

		typedef map<unsigned int,Context*> Contexts;
		static Contexts contexts_;
		typedef list<Context*> IdleContexts;
		static IdleContexts idle_contexts_;
		static pthread_mutex_t context_mutex_;
		static unsigned int context_idx_;
	public: // data
		Context(): apply_client_(NULL),register_client_(NULL),node_type_(SourceNode),
			node_id_(INVALID_ID),auto_delete_time_(0),path_(""),zkhanlde_(NULL),context_id_(0){}
		IZkApplyClient::ZkApplyClientImpl* apply_client_;
		IZkRegisterClient::ZkRegisterClientImpl* register_client_;
		NodeType node_type_;
		NodeID node_id_;
		unsigned auto_delete_time_;
		string path_;
		zhandle_t* zkhanlde_;
		bool is_idle_;
		unsigned int context_id_;

	};
	list<Context*> Context::idle_contexts_;
	map<unsigned int,Context*> Context::contexts_;
	pthread_mutex_t Context::context_mutex_;
	unsigned int Context::context_idx_ = 0;
	ZkAutoInit context_auto_init( &Context::context_mutex_ );


	struct ObjectInfo 
	{
		IZkRegisterClient* register_client_;
		IZkApplyClient* apply_client_;
	};

	// 安全的对象管理类
	class IObjectContainer
	{		
	public:
		static void AddObject( IZkRegisterClient* client )
		{
			ZkAutoLock lock( &IObjectContainer::mutex_ );
			ObjectInfo object_info;
			object_info.register_client_ = client;
			objects_.push_back( object_info );
		}
		static void AddObject( IZkApplyClient* client )
		{
			ZkAutoLock lock( &IObjectContainer::mutex_ );
			ObjectInfo object_info;
			object_info.apply_client_ = client;
			objects_.push_back( object_info );
		}

		static void Destory( IZkRegisterClient* client )
		{
			ZkAutoLock lock( &IObjectContainer::mutex_ );

			Context::Clear( client );

			Objects::iterator itr = objects_.begin();
			while ( itr != objects_.end() )
			{
				if ( itr->register_client_ == client )
				{
					objects_.erase( itr );
					return;
				}
				itr++;
			}
		}
		static void Destory( IZkApplyClient* client )
		{
			ZkAutoLock lock( &IObjectContainer::mutex_ );

			Context::Clear( client );

			Objects::iterator itr = objects_.begin();
			while ( itr != objects_.end() )
			{
				if ( itr->apply_client_ == client )
				{				
					objects_.erase( itr );
					return;
				}
				itr++;
			}
		}

	public:
		static pthread_mutex_t mutex_;
		typedef vector<ObjectInfo> Objects;
		static Objects objects_;
	};

	pthread_mutex_t IObjectContainer::mutex_;
	vector<ObjectInfo> IObjectContainer::objects_;
	ZkAutoInit auto_init_(&IObjectContainer::mutex_);

	IZkRegisterClient* IZkRegisterClient::Create(ZkCallback callback, void* context /* = NULL */,
		char* root_path /* = "/Resource" */, char* source_path /* = "/Source" */ )
	{
		zoo_set_debug_level(ZOO_LOG_LEVEL_ERROR);
		
		IZkRegisterClient* client = new IZkRegisterClient( callback, context, root_path, source_path );

		IObjectContainer::AddObject( client );

		return client;

	}
	void IZkRegisterClient::Destory( IZkRegisterClient* client )
	{
		IObjectContainer::Destory( client );
		
		ZkAutoLock lock( &IObjectContainer::mutex_ );
		DEL_PTR( client );
	}

	IZkRegisterClient::IZkRegisterClient( ZkCallback callback, void* context /* = NULL */,
		char* root_path /* = "/Resource" */, char* source_path /* = "/Source" */)
	{
		impl_ = new ZkRegisterClientImpl( callback, context, root_path, source_path, this );
	}
	IZkRegisterClient::~IZkRegisterClient(){
		DEL_PTR(impl_)
	}

	bool IZkRegisterClient::Connect( const char* host, int time_out )
	{
		return impl_->Connect( host, time_out );
	}

	int IZkRegisterClient::DisConnect()
	{
		return impl_->DisConnect();
	}

	int IZkRegisterClient::Register( const char* res, NodeValue* value, NodeID& id )
	{
		return impl_->Register( res, value, id );
	}
	int IZkRegisterClient::Change( NodeID& id, NodeValue* value )
	{
		return impl_->Change( id, value );
	}
	int IZkRegisterClient::Delete( NodeID& id )
	{
		return impl_->Delete( id );
	}
	ZkSystemState IZkRegisterClient::GetSystemState()
	{
		return impl_->GetSystemState();
	}

	void IZkRegisterClient::Print()
	{
		impl_->Print();
	}

	void IZkRegisterClient::ZkRegisterClientImpl::Print()
	{
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "============ register client info begin ============\n" );
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "source path = %s\n",source_path_.c_str() );
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "system state = %d\n",system_state_ );
		Nodes::iterator itr = nodes_.begin();
		while ( itr != nodes_.end() )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "noid = %d \n",itr->first );
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "path = %s \n", itr->second->GetPath() );
			
			if ( itr->second != NULL )
			{
				NodeValue* value = itr->second->GetValue();

				if ( value != NULL )
				{
					int count = value->GetCount();
					for ( int i = 0; i < count; i++ )
					{
						ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "     %s : %s \n", value->GetKey( i ), value->GetValue( i ) );
					}
				}
			}	
			itr++;
		}

		Context::Print();
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "============  register client info end  ============\n" );
	}

	bool IZkRegisterClient::ZkRegisterClientImpl::Init( bool bInit /* = true */ )
	{
		bool bRet = false;
		if ( bInit == false )
		{
			bRet = pthread_mutex_destroy( &mutex_ );
		}
		else
		{
			pthread_mutexattr_t attr;
			pthread_mutexattr_init( &attr );	
			pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
			bRet = pthread_mutex_init( &mutex_, &attr );		
		}
		return bRet;
	}

	bool IZkRegisterClient::ZkRegisterClientImpl::Connect( const char* host, int time_out )
	{
		if ( zkhandle_ != NULL )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"zkhandle is not null\n");
			return false;
		}
		
		if ( connect_context_ == NULL )
		{
			connect_context_ = Context::Create( this );
		}

		zkhandle_ = zookeeper_init( host, IZkRegisterClient::ZkRegisterClientImpl::Watch, time_out, 0, (void*)connect_context_->context_id_, 0 );
		if ( zkhandle_ != NULL )
		{
			system_state_ = zkConnecting;
		}
		else
		{
			Context::Destory( connect_context_ );
			connect_context_ = NULL;
		}
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"init zookeeper zkhandle=%d\n", zkhandle_ );
		return (zkhandle_!=NULL);
	}

	bool IZkRegisterClient::ZkRegisterClientImpl::ReConnect( const char* host, int time_out /* = 10000 */ )
	{
		if ( zkhandle_ != NULL )
		{
			zookeeper_close( zkhandle_ );
		}

		if ( connect_context_ == NULL )
		{
			connect_context_ = Context::Create( this );
		}

		zkhandle_ = zookeeper_init( host, IZkRegisterClient::ZkRegisterClientImpl::Watch, time_out, 0, (void*)connect_context_->context_id_, 0 );
		if ( zkhandle_ != NULL )
		{
			system_state_ = zkReConnecting;
		}
		else
		{
			Context::Destory( connect_context_ );
			connect_context_ = NULL;
		}
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"zookeeper reconnecting\n" );
		return (zkhandle_!=NULL);
	}

	bool IZkRegisterClient::ZkRegisterClientImpl::RestoreRegisteredNode()
	{
		return false;
	}

	int IZkRegisterClient::ZkRegisterClientImpl::DisConnect()
	{
		ZkAutoLock lock( &mutex_ );
		system_state_ = zkDisconnect;
		RemoveAllNode();
		if ( zkhandle_ != NULL )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"close zookeeper\n" );
			int ret = zookeeper_close( zkhandle_ );
			zkhandle_ = NULL;
			return ret;
		}
		return -1;
	}

	int IZkRegisterClient::ZkRegisterClientImpl::Register( const char* res, NodeValue* value, NodeID& id )
	{
		ZkAutoLock lock( &mutex_ );
		if ( system_state_ != zkConnected )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"Register is fail system_state_=%d\n", system_state_);
			return -1;
		}

		int len = MAX_BUFF;
		char buffer[MAX_BUFF];
		bool serialize_result = value->Serialize( buffer, len );
		if ( serialize_result == false )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"Register is fail buff too long length=%d\n", MAX_BUFF);
			return -1;
		}

		string path = root_path_  ;
		path += "/";	
		path += res;
		path += source_path_;
		path += "/";
		path += res;

		id = ++index_;
		Context* context = Context::Create( this, res, id );// = new Context( this, res, id );

		int ret = zoo_acreate( zkhandle_, path.c_str(), buffer, len, &ZOO_OPEN_ACL_UNSAFE, 
			ZOO_EPHEMERAL|ZOO_SEQUENCE , IZkRegisterClient::ZkRegisterClientImpl::StringCB, (void*)context->context_id_ );

		if ( ret == ZOK )
		{
			NodeInfo* node_info = new NodeInfo();
			node_info->SetPath("");
			node_info->SetValue( value );
			nodes_[id] = node_info;
		}
		else
		{
			Context::Destory( context );
			id = INVALID_ID;
		}
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"Register source path= %s ret=%d\n", path.c_str(),ret );
	
		return ret;
	}

	int IZkRegisterClient::ZkRegisterClientImpl::Change( NodeID& id, NodeValue* value )
	{
		ZkAutoLock lock( &mutex_ );
		if ( system_state_ != zkConnected )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"Change is fail system_state_=%d\n", system_state_);
			return -1;
		}
		
		NodeInfo* node = GetResInfo( id );
		if ( node == NULL )
		{
			return -1;
		}
		int len = MAX_BUFF;
		char buffer[MAX_BUFF];
		bool serialize_result = value->Serialize( buffer, len );
		if ( serialize_result == false )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"Change is fail buff too long length=%d\n", MAX_BUFF );
			return -1;
		}

		Context* context = Context::Create( this, node->GetPath() );
		int ret = zoo_aset( zkhandle_, node->GetPath(), buffer, len, -1, IZkRegisterClient::ZkRegisterClientImpl::StatCB,(void*)context->context_id_ );

		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"Change source path= %s ret=%d\n", node->GetPath(), ret );
		if ( ret != ZOK )
		{
			Context::Destory( context );
		}
		else
		{
			node->SetValue( value );
		}

		return ret;
	}

	int IZkRegisterClient::ZkRegisterClientImpl::Delete( NodeID& id )
	{
		if ( system_state_ != zkConnected )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "Delete is fail system_state_=%d\n", system_state_);
			return -1;
		}
		
		NodeInfo* node = GetResInfo( id );
		Context* context = Context::Create( this, node->GetPath() );
		int ret = zoo_adelete( zkhandle_, node->GetPath(), -1, IZkRegisterClient::ZkRegisterClientImpl::DeleteCB, (void*)context->context_id_ );

		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "delete source path = %s ret=%d\n ", node->GetPath(), ret );
		if ( ret != ZOK )
		{
			Context::Destory( context );
		}
		else
		{
			RemoveNodeInfo( id );
		}
		return ret;
	}

	NodeInfo* IZkRegisterClient::ZkRegisterClientImpl::GetResInfo( NodeID id )
	{
		Nodes::iterator itr = nodes_.find( id );
		if ( itr != nodes_.end() )
		{
			// 如果路径为空，表明zk服务器还没有回复	
			if ( itr->second->IsCreated() )
			{
				return itr->second;
			}
		}
		return NULL;
	}

	void IZkRegisterClient::ZkRegisterClientImpl::Watch(zhandle_t *zh, int type, int state, const char *path,void *watcherCtx)
	{
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "Watch type = %d state=%d \n ", type, state );
		ZkAutoLock lock( &IObjectContainer::mutex_ );
		if ( type == ZOO_SESSION_EVENT )
		{		
			unsigned index = (unsigned int)(watcherCtx);

			Context context;
			if ( Context::GetContext( index, context ) )
			{
				if ( state == ZOO_CONNECTED_STATE )
				{
					context.register_client_->OnConnected();
				}
				else if ( state == ZOO_EXPIRED_SESSION_STATE )
				{	
					context.register_client_->OnDisconnected();
				}
				else if ( state == ZOO_CONNECTING_STATE )
				{
					context.register_client_->OnConnecting();
				}	
			}		
		}
	}

	void IZkRegisterClient::ZkRegisterClientImpl::StringCB(int rc, const char *value, const void *data)
	{
		ZkAutoLock lock( &IObjectContainer::mutex_ );
		unsigned index = (unsigned int)(data);
		Context context;
		if ( !Context::GetContext( index, context ) )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS," StringCB context is null\n" );
			return ;
		}

		context.register_client_->OnRegisterRsp( rc, value, context.node_id_ );
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"register callback rc=%d path=%s id=%d\n", rc, value,context.node_id_ );
		
		Context::Destory(index);	
	}

	void IZkRegisterClient::ZkRegisterClientImpl::DeleteCB(int rc, const void *data)
	{
		ZkAutoLock lock( &IObjectContainer::mutex_ );

		unsigned index = (unsigned int)(data);
		Context context;
		if ( !Context::GetContext( index, context ) )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS," DeleteCB context is null\n" );
			return;
		}		
		
		context.register_client_->OnDeleteRsp( rc, context.path_.c_str(),context.node_id_ );
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"delete callback rc=%d path=%s id=%d\n", rc, context.path_.c_str(),context.node_id_ );
		
		Context::Destory(index);			
	}

	void IZkRegisterClient::ZkRegisterClientImpl::StatCB(int rc, const struct Stat *stat, const void *data )
	{
		ZkAutoLock lock( &IObjectContainer::mutex_ );
		unsigned index = (unsigned int)(data);
		Context context;
		if ( !Context::GetContext( index, context ) )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS," StatCB context is null\n" );
			return;
		}
	
		context.register_client_->OnChangeRsp( rc, context.path_.c_str(), context.node_id_ );
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"change callback rc=%d path=%s id=%d\n", rc, context.path_.c_str(),context.node_id_ );
	
		Context::Destory(index);	
	}

	void IZkRegisterClient::ZkRegisterClientImpl::OnRegisterRsp( int rc, const char* value, NodeID id )
	{
		ZkAutoLock lock(&mutex_);
		if ( value != NULL )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS," OnRegisterRsp rc=%d value=%s id=%d\n", rc, value, id );
		}
		else
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS," OnRegisterRsp rc=%d value=null id=%d\n", rc, id );
		}
		
		if ( rc != ZOK )
		{
			RemoveNodeInfo( id );
		}
		else
		{
			UpdateNodeInfo( id, value );
		}
		if ( callback_ != NULL )
		{
			CallbackParam param;
			param.type = RegisterCb;
			param.result = rc;
			param.register_param.id = id;
			param.context = callback_context_;
			callback_( &param );
		}
	}

	void IZkRegisterClient::ZkRegisterClientImpl::OnChangeRsp( int rc, const char* value, NodeID id )
	{
		ZkAutoLock lock(&mutex_);
		if ( value != NULL )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS," OnChangeRsp rc=%d value=%s id=%d\n", rc, value, id );
		}
		else
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS," OnChangeRsp rc=%d value=null id=%d\n", rc, id );
		}
		if ( callback_ != NULL )
		{
			CallbackParam param;
			param.type = ChangeCb;
			param.result = rc;
			param.register_param.id = id;
			param.context = callback_context_;
			callback_( &param );
		}
	}

	void IZkRegisterClient::ZkRegisterClientImpl::OnDeleteRsp( int rc, const char* value, NodeID id )
	{
		ZkAutoLock lock(&mutex_);
		if ( value != NULL )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS," OnDeleteRsp rc=%d value=%s id=%d\n", rc, value, id );
		}
		else
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS," OnDeleteRsp rc=%d value=null id=%d\n", rc, id );
		}
		RemoveNodeInfo( id );
		if ( callback_ != NULL )
		{
			CallbackParam param;
			param.type = DeleteCb;
			param.result = rc;
			param.register_param.id = id;
			param.context = callback_context_;
			callback_( &param );
		}
	}

	void IZkRegisterClient::ZkRegisterClientImpl::UpdateNodeInfo( NodeID id, const char* path )
	{
		Nodes::iterator itr = nodes_.find( id );
		if ( itr != nodes_.end() )
		{
			itr->second->SetPath( path );
			itr->second->SetNodeState( emNormal );
		}
	}

	void IZkRegisterClient::ZkRegisterClientImpl::RemoveNodeInfo( NodeID id )
	{
		Nodes::iterator itr = nodes_.find( id );
		if ( itr != nodes_.end() )
		{
			DEL_PTR( itr->second )
			nodes_.erase( itr );
		}
	}

	void IZkRegisterClient::ZkRegisterClientImpl::OnConnected()
	{
		ZkAutoLock lock( &mutex_ );
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS," OnConnected \n");
		ZkSystemState old_state = system_state_;	
		system_state_ = zkConnected;
		
		if ( callback_ != NULL )
		{
			CallbackParam param;
			if ( old_state == zkReConnecting )
			{
				param.type = ReConnectCb;
			}
			else
			{			
				param.type = ConnectCb;
			}
			param.result = 0;
			param.context = callback_context_;
			callback_( &param );
		}
	}

	void IZkRegisterClient::ZkRegisterClientImpl::OnConnecting()
	{
		ZkAutoLock lock( &mutex_ );
		system_state_ = zkReConnecting;
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS," OnConnecting \n");

		if ( callback_ != NULL )
		{
			CallbackParam param;
			param.type = ReConnectingCb;
			param.result = 0;
			param.context = callback_context_;
			callback_( &param );
		}
	}

	void IZkRegisterClient::ZkRegisterClientImpl::OnDisconnected()
	{
		ZkAutoLock lock( &mutex_ );
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS," OnDisConnected \n");
		DisConnect();

		if ( callback_ != NULL )
		{
			CallbackParam param;
			param.type = DisconnectCb;
			param.result = 0;
			param.context = callback_context_;
			callback_( &param );
		}
	}

	void IZkRegisterClient::ZkRegisterClientImpl::RemoveAllNode()
	{
		Nodes::iterator itr = nodes_.begin();
		while ( itr != nodes_.end() )
		{
			DEL_PTR( itr->second )
			itr++;
		}
		nodes_.clear();
	}

/************************************** apply client **********************************************/

	IZkApplyClient* IZkApplyClient::Create(char* res_type, 
		ZkCallback callback,
		void* context /* = NULL */, 
		char* root_name /* = "/Resource" */, 
		char* apply_queue_name /* = "/ApplyQueue" */, 
		char* reserve_queue_name /* = "/ReserveQueue" */, 
		char* source_name /* = "/Source" */)
	{
		zoo_set_debug_level(ZOO_LOG_LEVEL_ERROR);
		IZkApplyClient* client = new IZkApplyClient( res_type, 
			callback,
			context,
			root_name, 
			apply_queue_name, 
			reserve_queue_name, 
			source_name );

		IObjectContainer::AddObject( client );

		return client;
	}

	void IZkApplyClient::Destory( IZkApplyClient* client )
	{
		IObjectContainer::Destory( client );
		ZkAutoLock lock( &IObjectContainer::mutex_ );
		DEL_PTR( client );
	}

	IZkApplyClient::IZkApplyClient( char* res_type, 
		ZkCallback callback,
		void* context  /* = NULL */, 
		char* root_name /* = "/Resource" */, 
		char* apply_queue_name /* = "/ApplyQueue" */, 
		char* reserve_queue_name /* = "/ReserveQueue" */, 
		char* source_name /* = "/Source" */ )
	{
		impl_ = new ZkApplyClientImpl( res_type, callback, context,
			root_name, apply_queue_name, reserve_queue_name, source_name,this );
	}

	IZkApplyClient::~IZkApplyClient(void)
	{
		DEL_PTR(impl_);
	}

	bool IZkApplyClient::Connect( const char* host, int time_out /* = 10000 */ )
	{
		return impl_->Connect( host, time_out );
	}

	int IZkApplyClient::Apply( int time_out /* = 10000 */ )
	{
		return impl_->Apply( time_out );
	}

	ZkSystemState IZkApplyClient::GetSystemState()
	{
		return impl_->GetSystemState();
	}
	
	int IZkApplyClient::Disconnect()
	{
		return impl_->Disconnect();
	}

	void IZkApplyClient::Print()
	{
		impl_->Print();
	}

	void IZkApplyClient::ZkApplyClientImpl::Print()
	{
		ZkAutoLock lock( &mutex_ );
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "============ apply client info begin ============\n" );
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "source type = %s\n",res_type_.c_str() );
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "apply path = %s\n", apply_path_.c_str() );
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"system state = %d\n",system_state_ );
		
		Sources::iterator itr = sources_.begin();
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "------------> source info\n" );
		while ( itr != sources_.end() )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "source path = %s \n",itr->first.c_str() );
			
			NodeValue* value = itr->second;
			if ( value != NULL )
			{
				int count = value->GetCount();
				for ( int i = 0; i < count; i++ )
				{
					ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "     %s : %s \n", value->GetKey( i ), value->GetValue( i ) );
				}
			}	
			itr++;
		}
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "\n------------> reserve queue info\n" );
		ReserveQueue::iterator itr_reserve = reserve_queue_.begin();
		while ( itr_reserve != reserve_queue_.end() )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "source path = %d \n",itr_reserve->first.c_str() );

			NodeValue* value = itr_reserve->second;
			if ( value != NULL )
			{
				int count = value->GetCount();
				for ( int i = 0; i < count; i++ )
				{
					ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "     %s : %s \n", value->GetKey( i ), value->GetValue( i ) );
				}
			}
			
			itr_reserve++;
		}

		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "\n------------> apply queue info\n" );
		
		Context::Print();
		
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "============  apply client info end  ============\n" );
	}

	bool IZkApplyClient::ZkApplyClientImpl::Connect( const char* host, int time_out )
	{
		if ( zkhandle_ != NULL )
		{
			return false;
		}

		if ( connect_context_ == NULL )
		{
			connect_context_ = Context::Create( zkhandle_, this );
		}

		zkhandle_ = zookeeper_init( host, IZkApplyClient::ZkApplyClientImpl::Watch, time_out, 0, (void*)connect_context_->context_id_, 0 );
		
		if ( zkhandle_ != NULL )
		{
			system_state_ = zkConnecting;
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"cli%d connect zookeeper ok \n" ,client_id_);
		}
		else
		{
			Context::Destory( connect_context_ );
			connect_context_ = NULL;
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"cli%d connect zookeeper fail \n" ,client_id_);
		}
		return ( zkhandle_ != NULL );	
	}

	bool IZkApplyClient::ZkApplyClientImpl::LoadSource()
	{
		if ( system_state_ != zkConnected  || zkhandle_ == NULL )
		{
			return false;
		}
	
		int ret = GetSourceList();
		if ( ret == ZOK )
		{
			ret = GetReserveList();
		}
		return (ret == ZOK);
	}


	int IZkApplyClient::ZkApplyClientImpl::Apply( unsigned time_out /* = 10000 */ )
	{
		ZkAutoLock lock( &mutex_ );
		if ( system_state_ != zkConnected || apply_state_ == applying || !is_inited_ )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"cli%d Apply fail system_state=%d apply_state=% is_inited=%d\n" ,client_id_, system_state_, apply_state_, (int)is_inited_);
			return -1;
		}
		
		Context* context = Context::Create( zkhandle_, this );
		string path = apply_queue_path_;
		path += "/";
		path += res_type_;
		int ret = zoo_acreate( zkhandle_, path.c_str(), NULL, -1, 
			&ZOO_OPEN_ACL_UNSAFE, ZOO_SEQUENCE|ZOO_EPHEMERAL,IZkApplyClient::ZkApplyClientImpl::ApplyNodeCB,(void*)context->context_id_ );

		if ( ret == ZOK )
		{
			apply_state_ = applying;
		}
		else
		{
			Context::Destory( context );
		}
		
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"cli%d apply source path=%s ret=%d\n", client_id_,path.c_str(), ret );

		return ret;
	}

	bool IZkApplyClient::ZkApplyClientImpl::GetApplyList()
	{
		if ( system_state_ != zkConnected || apply_state_ != applying )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"cli%d GetApplyList fail system_state=%d apply_state=% \n" ,client_id_, system_state_, apply_state_);
			return false;
		}
		Context* context = Context::Create( zkhandle_, this );
		if ( apply_list_watch_context_ == NULL )
		{
			apply_list_watch_context_ = Context::Create( zkhandle_, this );
		}
		int ret = zoo_awget_children( zkhandle_, apply_queue_path_.c_str(), IZkApplyClient::ZkApplyClientImpl::ApplyListChangeWatch, 
			(void*)apply_list_watch_context_->context_id_, IZkApplyClient::ZkApplyClientImpl::ApplyListNotifyCB, (void*)context->context_id_ );
		
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"cli%d get apply list path=%s ret = %d\n", client_id_, apply_queue_path_.c_str(), ret );

		if ( ret != ZOK )
		{
			Context::Destory( context );
		}
		return (ret != ZOK);
	}

	bool IZkApplyClient::ZkApplyClientImpl::UpdateApplyList( int rc, const struct String_vector *strings )
	{
		ZkAutoLock lock( &mutex_ );
		// state=applying的时候，有一种可能apply节点还没有回复，path="" 这个时候也不能进行处理
		if ( system_state_ != zkConnected || apply_state_ != applying || apply_path_ == "" )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"cli%d UpdateApplyList fail system_state=%d apply_state=%d \n" ,client_id_, system_state_, apply_state_);
			return false;
		}
		if ( rc == ZOK )
		{
			if ( IsFirstPos( apply_path_.c_str(), strings ) )
			{
				ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "cli%d client choose source path=%s\n", client_id_,apply_path_.c_str() );
				return DoChoice();
			}
		}
		return false;
	}

	bool IZkApplyClient::ZkApplyClientImpl::IsFirstPos( const char* path,  const struct String_vector *strings )
	{
		if ( path == "" )
		{
			return false;
		}
		int size = res_type_.size() + reserve_queue_path_.size();
		string path_nbr = path+size;
		int nbr = atoi( path_nbr.c_str() );

		for ( int i = 0; i < strings->count; i++ )
		{
			string temp_str = strings->data[i] + res_type_.size();
			int temp_nbr = atoi( temp_str.c_str() );
			if ( temp_nbr < nbr )
			{
				return false;
			}
		}
		return true;
	}

	bool IZkApplyClient::ZkApplyClientImpl::UpdateApplyNode( int rc, const char* path )
	{
		ZkAutoLock lock( &mutex_ );
		if ( rc == ZOK )
		{
			apply_path_ = path;
			bool bRet = GetApplyList();
			
			if ( callback_ != NULL )
			{
				CallbackParam param;
				param.type = ApplyAckCb;
				param.result = rc;
				param.context = callback_context_;
				param.apply_ack_param.full_path = path;

				int size = res_type_.size() + reserve_queue_path_.size();
				string path_nbr = path + size;
				int nbr = atoi( path_nbr.c_str() );

				param.apply_ack_param.index = nbr;

				callback_( &param );
			}

			return bRet;
		}
		else
		{
			EndApply();
	
			if ( callback_ != NULL )
			{
				CallbackParam param;
				param.type = ApplyFailCb;
				param.result = rc;
				param.context = callback_context_;
				callback_( &param );
			}
		}
		return false;
	}

	bool IZkApplyClient::ZkApplyClientImpl::DoChoice()
	{
		EndApply();
		if ( callback_ != NULL )
		{
			int reserve_size = reserve_queue_.size();
			int source_size = sources_.size();

		
			NodeValue **reserve_buffer = new NodeValue*[reserve_size]; 
			NodeValue **source_buffer = new NodeValue*[source_size]; 

			ReserveQueue::iterator reserve_itr = reserve_queue_.begin();
			int index = 0;
			while ( reserve_itr != reserve_queue_.end() )
			{
				reserve_buffer[index] = reserve_itr->second;
				reserve_itr++;
				index++;
			}

			index = 0;
			Sources::iterator source_itr = sources_.begin();
			while ( source_itr != sources_.end() )
			{
				source_buffer[index] = source_itr->second;
				source_itr++;
				index++;
			}

			//unsigned auto_delete_time = 0;
			NodeValue* value = NodeValue::Create();
			CallbackParam param;
			param.type = ApplySuccessCb;
			param.apply_success_param.source_values = source_buffer;
			param.apply_success_param.source_len = source_size;
			param.apply_success_param.reserve_values = reserve_buffer;
			param.apply_success_param.reserve_len = reserve_size;
			param.apply_success_param.auto_delete_time = 0;
			param.apply_success_param.reserve_value = value;
			param.apply_success_param.has_choosed = false;
			param.context = callback_context_;
			
			callback_( &param );

// 			bool ret = arbitration_callback_( source_buffer, source_size, reserve_buffer, 
// 				reserve_size, *value, auto_delete_time, callback_context_ );
			
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"cli%d user's choice is %d auto_delete=%d \n",client_id_, param.apply_success_param.has_choosed, param.apply_success_param.auto_delete_time );
			if ( param.apply_success_param.has_choosed )
			{
				Context* context = Context::Create( zkhandle_, this, param.apply_success_param.auto_delete_time );
				string path = reserve_queue_path_;
				path += "/";
				path += res_type_;

				int len = MAX_BUFF;
				char buffer[MAX_BUFF];
				bool bRet = value->Serialize( buffer, len );

				// 创建预占队列
				int ret = zoo_acreate( zkhandle_, path.c_str(), buffer, len, &ZOO_OPEN_ACL_UNSAFE, ZOO_SEQUENCE|ZOO_EPHEMERAL, 
					IZkApplyClient::ZkApplyClientImpl::ReserveNodeCreateCB, (void*)context->context_id_ );

				if ( ret != ZOK )
				{
					Context::Destory(context);
				}
				ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"cli%d create reserve node path=%s ret=%d \n",client_id_, path.c_str(), ret );
			}
			DEL_PTR_ARRAY(reserve_buffer)
			DEL_PTR_ARRAY(source_buffer)
			NodeValue::Destory( value );
		}				
		return true;
	}

	void IZkApplyClient::ZkApplyClientImpl::ApplyNodeCB(int rc, const char *value, const void *data)
	{	
		ZkAutoLock lock(&IObjectContainer::mutex_);
		unsigned index = (unsigned int)(data);
		Context context;
		if ( !Context::GetContext( index, context) )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"ApplyNodeCB Callback is null\n");
			return;
		}
		if ( value != NULL )
		{
			ZkClientPrint( ZK_LOG_LVL_DETAIL,"create apply node callback rc=%d path=%s\n", rc, value );
		}
		else
		{
			ZkClientPrint( ZK_LOG_LVL_DETAIL,"create apply node callback rc=%d path=null\n", rc );
		}
		
		context.apply_client_->UpdateApplyNode( rc, value );	
		Context::Destory( index );
	}

	void IZkApplyClient::ZkApplyClientImpl::ApplyListNotifyCB(int rc,const struct String_vector *strings, const void *data)
	{	
		ZkAutoLock lock(&IObjectContainer::mutex_);
		unsigned index = (unsigned int)(data);
		Context context;
		if ( !Context::GetContext( index, context ) )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"ApplyListNotifyCB Callback is null\n");
			return;
		}
	
		ZkClientPrint( ZK_LOG_LVL_DETAIL,"get apply list callback rc=%d\n", rc );
		context.apply_client_->UpdateApplyList( rc, strings );		
		Context::Destory( index );
	}

	void IZkApplyClient::ZkApplyClientImpl::ApplyListChangeWatch(zhandle_t *zh, int type, int state, const char *path,void *watcherCtx)
	{	
		ZkAutoLock lock(&IObjectContainer::mutex_);
		unsigned index = (unsigned int)watcherCtx;
		Context context;
		if ( !Context::GetContext( index, context ) )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"ApplyListChangeWatch context is null \n" );
			return;
		}
		Context* new_context = Context::Create( &context );
		int ret = zoo_awget_children( zh, path, IZkApplyClient::ZkApplyClientImpl::ApplyListChangeWatch,watcherCtx, IZkApplyClient::ZkApplyClientImpl::ApplyListNotifyCB, (void*)new_context->context_id_ );	
		
		if ( ret != ZOK )
		{
			Context::Destory( new_context );
		}
		if ( path != NULL )
		{
			ZkClientPrint( ZK_LOG_LVL_DETAIL,"ApplyListChangeWatch type=%d path=%s ret=%d\n", type, path, ret );
		}
		else
		{
			ZkClientPrint( ZK_LOG_LVL_DETAIL,"ApplyListChangeWatch type=%d path=null ret=%d\n", type, ret );
		}
		
	}

	int IZkApplyClient::ZkApplyClientImpl::GetSourceList()
	{
		if ( source_list_wath_context_ == NULL )
		{
			source_list_wath_context_ = Context::Create( zkhandle_, this );
			source_list_wath_context_->node_type_ = SourceNode;
		}
		Context* context_source = Context::Create( zkhandle_, this, 0, source_path_, SourceNode );

		int ret = zoo_awget_children( zkhandle_, source_path_.c_str(), IZkApplyClient::ZkApplyClientImpl::ListChangeWatch, (void*)source_list_wath_context_->context_id_, 
			IZkApplyClient::ZkApplyClientImpl::ListNotifyCB, (void*)context_source->context_id_ );
		if ( ret != ZOK )
		{
			Context::Destory(context_source);	
			// 如果watcher无法设置，需要进行断链处理
			OnDisconnected();
		}
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"cli%d get source list path=%s result=%d \n",client_id_, source_path_.c_str(), ret );
		return ret;
	}

	int IZkApplyClient::ZkApplyClientImpl::GetSourceNode( const char* path )
	{
		if ( path != NULL )
		{
			Context* context_watch = Context::Create( zkhandle_,this, 0, path, SourceNode );
		/*	if ( source_node_wath_context_ == NULL )
			{
				source_node_wath_context_ = Context::Create( zkhandle_,this, 0, path, SourceNode );
			}*/
			Context* context = Context::Create( zkhandle_,this, 0, path, SourceNode );
			int ret = zoo_awget( zkhandle_, path, IZkApplyClient::ZkApplyClientImpl::NodeChangeWatch, (void*)context_watch->context_id_,
				IZkApplyClient::ZkApplyClientImpl::NodeNotifyCB, (void*)context->context_id_ );
			if ( ret != ZOK )
			{
				Context::Destory( context );
			}
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"cli%d get source node path=%s result=%d \n",client_id_, path , ret );
			return ret;
		}
		return -1;
	}

	int IZkApplyClient::ZkApplyClientImpl::UpdateSourceList( int rc, const struct String_vector* strings )
	{
		ZkAutoLock lock( &mutex_ );
		if ( rc == ZOK )
		{
			if ( strings != NULL )
			{		
				
				Sources::iterator itr = sources_.begin();
				while ( itr != sources_.end() )
				{
					bool is_find = false;
					for ( int i = 0; i < strings->count; i++ )
					{
						string path = source_path_;
						path += "/";
						path += strings->data[i];

						if ( itr->first == path )
						{
							is_find = true;
							break;
						}
					}
					if ( is_find )
					{
						itr++;
					}
					else
					{
						NodeValue::Destory( itr->second );
						sources_.erase( itr++ );
					}
				}


				for ( int i = 0; i < strings->count; i++ )
				{
					string path = source_path_;
					path += "/";
					path += strings->data[i];

					Sources::iterator itr = sources_.find( path );

					if ( itr == sources_.end() )
					{
						sources_[path] = NULL;
						int ret = GetSourceNode( path.c_str() );
						if ( ret != ZOK )
						{
							itr = sources_.find( path );
							if ( itr != sources_.end() )
							{
								sources_.erase( itr );
							}
						}		
					}
				}

				// 通知列表更新（仅有列表所有初始化成功）
				NotifySourceList();
			}	
		}
		return rc;
	}

	void IZkApplyClient::ZkApplyClientImpl::NotifySourceList()
	{			
		Sources::iterator source_itr = sources_.begin();
		while ( source_itr != sources_.end() )
		{
			if ( source_itr->second == NULL )
			{
				return;
			}
			source_itr++;
		}

		source_itr = sources_.begin();
		int source_size = sources_.size();
		NodeValue **source_buffer = new NodeValue*[source_size]; 
		int index = 0;		
		while ( source_itr != sources_.end() )
		{
			source_buffer[index] = source_itr->second;
			source_itr++;
			index++;
		}

		CallbackParam param;
		param.type = SourceChangeCb;
		param.source_change_param.values = source_buffer;
		param.source_change_param.len = source_size;	
		param.context = callback_context_;
		callback_( &param );	
	
		DEL_PTR_ARRAY( source_buffer );
	}

	int IZkApplyClient::ZkApplyClientImpl::UpdateSourceNode( int rc, const char *value, int value_len, const char* path )
	{
		ZkAutoLock lock( &mutex_ );
		if ( rc == ZOK )
		{
			Sources::iterator itr = sources_.find( path );
			NodeValue* node_value = NULL;
			if ( itr != sources_.end() )
			{			
				if ( itr->second == NULL )
				{
					itr->second = NodeValue::Create();
				}
				node_value = itr->second;
			}
			else
			{
				node_value = NodeValue::Create();
			}
			node_value->DeSerialize( value, value_len );
			sources_[path] = node_value;

			NotifySourceList();
		}
		else
		{
			Sources::iterator itr = sources_.find( path );
			if ( itr != sources_.end() )
			{
				if ( itr->second != NULL )
				{
					NodeValue::Destory( itr->second );
				}
				sources_.erase( itr );
			}
		}
		return rc;
	}

	int IZkApplyClient::ZkApplyClientImpl::GetReserveList()
	{
		if ( reserve_list_watch_context_ == NULL )
		{
			reserve_list_watch_context_= Context::Create( zkhandle_, this, 0, reserve_queue_path_, ReserveNode ); 
		}
		Context* context_source = Context::Create( zkhandle_, this, 0, reserve_queue_path_, ReserveNode ); 
		
		int ret = zoo_awget_children( zkhandle_, reserve_queue_path_.c_str(), IZkApplyClient::ZkApplyClientImpl::ListChangeWatch, 
			(void*)reserve_list_watch_context_->context_id_ , IZkApplyClient::ZkApplyClientImpl::ListNotifyCB, (void*)context_source->context_id_ );
		if ( ret != ZOK )
		{
			Context::Destory( context_source );
		}
		ZkClientPrint( ZK_LOG_LVL_DETAIL,"cli%d get reserve list path=%s result=%d \n",client_id_, reserve_queue_path_.c_str() , ret );
		return ret;
	}

	int IZkApplyClient::ZkApplyClientImpl::GetReserveNode( const char* path )
	{
		if ( path != NULL )
		{
			/*if ( reserve_node_watch_context_ == NULL )
			{
				reserve_node_watch_context_ = Context::Create( zkhandle_, this, 0, path, ReserveNode );
			}*/
			Context* context_watch = Context::Create( zkhandle_, this, 0, path, ReserveNode );
			Context* context = Context::Create( zkhandle_, this, 0, path, ReserveNode );
		
			int ret = zoo_awget( zkhandle_, path, IZkApplyClient::ZkApplyClientImpl::NodeChangeWatch, (void*)context_watch->context_id_, 
				IZkApplyClient::ZkApplyClientImpl::NodeNotifyCB, (void*)context->context_id_ );
			if ( ret != ZOK )
			{
				Context::Destory( context );
			}
			ZkClientPrint( ZK_LOG_LVL_DETAIL,"cli%d get reserve node path=%s result=%d applystate=%d\n",client_id_, path , ret, apply_state_ );
		}
		return ZOK;
	}
	int IZkApplyClient::ZkApplyClientImpl::UpdateReserveList( int rc, const struct String_vector* strings )
	{
		ZkAutoLock lock( &mutex_ );
		if ( rc == ZOK )
		{
			if ( strings != NULL )
			{		
				ReserveQueue::iterator itr = reserve_queue_.begin();
				while ( itr != reserve_queue_.end() )
				{
					bool is_find = false;
					for ( int i = 0; i < strings->count; i++ )
					{
						string path = reserve_queue_path_;
						path += "/";
						path += strings->data[i];
						if ( itr->first == path )
						{
							is_find = true;
							break;
						}
					}
					if ( is_find )
					{
						itr++;
					}
					else
					{
						NodeValue::Destory( itr->second );
						reserve_queue_.erase( itr++ );
					}
				}
				for ( int i = 0; i < strings->count; i++ )
				{
					string path = reserve_queue_path_;
					path += "/";
					path += strings->data[i];
					GetReserveNode( path.c_str() );
				}
			}
			else
			{
				RemoveReserveNode(NULL);
			}
			if ( is_inited_ == false && callback_ != NULL )
			{
				is_inited_ = true;
				CallbackParam param;
				param.type = ApplyInited;
				param.context = callback_context_;
				callback_( &param );
			}		
		}
		else
		{
			// 如果获取失败，需要重新获取
			GetReserveList();
		}
		return rc;
	}

	int IZkApplyClient::ZkApplyClientImpl::UpdateReserveNode( int rc, const char *value, int value_len, const char* path )
	{
		ZkAutoLock lock( &mutex_ );
		if ( rc == ZOK )
		{
			ReserveQueue::iterator itr = reserve_queue_.find( path );
			NodeValue* node_value = NULL;
			if ( itr != reserve_queue_.end() )
			{
				node_value = itr->second;
			}
			else
			{
				node_value = NodeValue::Create();
			}

			node_value->DeSerialize( value, value_len );
			reserve_queue_[path] = node_value;
		}
		return rc;
	}

	void IZkApplyClient::ZkApplyClientImpl::ListNotifyCB(int rc,const struct String_vector *strings, const void *data)
	{
		ZkAutoLock lock(&IObjectContainer::mutex_);
		unsigned index = (unsigned int)data;
		Context context;
		if ( !Context::GetContext( index, context ) )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"ListNotifyCB Callback is null\n");
			return;
		}

		if ( context.node_type_ == SourceNode )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"get source list callback rc=%d\n", rc );
				context.apply_client_->UpdateSourceList( rc, strings );
		}
		else if ( context.node_type_ == ReserveNode )
		{
			ZkClientPrint( ZK_LOG_LVL_DETAIL,"get reserve list callback rc=%d\n", rc  );
				context.apply_client_->UpdateReserveList( rc, strings );
		}
		
		Context::Destory( index );
	}

	void IZkApplyClient::ZkApplyClientImpl::NodeNotifyCB(int rc, const char *value, int value_len, const struct Stat *stat, const void *data)
	{
		ZkAutoLock lock(&IObjectContainer::mutex_);
		unsigned index = (unsigned int)data;
		Context context;
		if ( !Context::GetContext( index, context ) )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"NodeNotifyCB Callback is null\n");
			return;
		}
		
		if ( context.node_type_ == SourceNode )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"get source node callback rc=%d path=%d \n", rc ,context.path_.c_str() );
				context.apply_client_->UpdateSourceNode( rc, value, value_len, context.path_.c_str() );
		}
		else if ( context.node_type_ == ReserveNode )
		{
			ZkClientPrint( ZK_LOG_LVL_DETAIL,"get reserve node callback rc=%d path=%d \n", rc ,context.path_.c_str() );
				context.apply_client_->UpdateReserveNode( rc, value, value_len, context.path_.c_str() );
		}

		Context::Destory( index );
	}

	void IZkApplyClient::ZkApplyClientImpl::ListChangeWatch(zhandle_t *zh, int type, int state, const char *path,void *watcherCtx)
	{
		ZkAutoLock lock(&IObjectContainer::mutex_);
		unsigned index = (unsigned int)watcherCtx;
		Context context;
		if ( !Context::GetContext( index, context ) )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"ListChangeWatch context fail \n" );
			return;
		}
		Context* context_cb = Context::Create( &context );
		int ret = zoo_awget_children( zh, path, IZkApplyClient::ZkApplyClientImpl::ListChangeWatch,watcherCtx, 
			IZkApplyClient::ZkApplyClientImpl::ListNotifyCB,(void*)context_cb->context_id_ );
		if ( ret != ZOK )
		{
			Context::Destory( context_cb );
		}

		ZkClientPrint( ZK_LOG_LVL_DETAIL,"list change  path=%d \n", path );
	}

	void IZkApplyClient::ZkApplyClientImpl::NodeChangeWatch(zhandle_t *zh, int type, int state, const char *path,void *watcherCtx)
	{
		ZkAutoLock lock(&IObjectContainer::mutex_);
		unsigned index = (unsigned int)watcherCtx;
		Context context;
		if ( !Context::GetContext( index, context ) )
		{
			return;
		}
		Context* context_cb = Context::Create( &context );
		int ret = zoo_awget( zh, path, IZkApplyClient::ZkApplyClientImpl::NodeChangeWatch, watcherCtx, 
			IZkApplyClient::ZkApplyClientImpl::NodeNotifyCB, (void*)context_cb->context_id_ );
		if ( ret != ZOK )
		{
			Context::Destory( context_cb );
		}
		ZkClientPrint( ZK_LOG_LVL_DETAIL,"node change  path=%d ret=%d\n", path, ret );
	}

	int IZkApplyClient::ZkApplyClientImpl::Disconnect()
	{
		ZkAutoLock lock( &mutex_ );
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"Disconnect call\n");
		system_state_ = zkDisconnect;
		apply_state_ = idle;
		is_inited_ = false;
		RemoveSourceNode( NULL );
		RemoveReserveNode( NULL );
		if ( zkhandle_ )
		{
			int ret = zookeeper_close( zkhandle_ );
			zkhandle_ = NULL;
			return ret;
		}
		return -1;
	}

	void IZkApplyClient::ZkApplyClientImpl::EndApply()
	{
		// 删除申请队列
		int ret = zoo_adelete( zkhandle_, apply_path_.c_str(), -1, IZkApplyClient::ZkApplyClientImpl::VoidCB, (void*)this );

		apply_state_ = idle;

		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"cli%d delete apply node path=%s ret=%d\n", client_id_, apply_path_.c_str(), ret );

		apply_path_ = "";		
	}


	void IZkApplyClient::ZkApplyClientImpl::Watch(zhandle_t *zh, int type, int state, const char *path,void *watcherCtx)
	{
		ZkAutoLock lock(&IObjectContainer::mutex_);
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"ApplyClient Watch type=%d state=%d \n", type, state );

		
		if ( type == ZOO_SESSION_EVENT )
		{
			
			unsigned index = (unsigned int)watcherCtx;
			Context context;
			if ( !Context::GetContext( index, context ) )
			{
				ZkClientPrint( ZK_LOG_LVL_KEYSTATUS, "ApplyClient Watch context is null\n" );
				return;
			}

			if ( state == ZOO_CONNECTED_STATE )
			{
				context.apply_client_->OnConnected();
			}
			else if ( state == ZOO_EXPIRED_SESSION_STATE )
			{
				context.apply_client_->OnDisconnected();
			}
			else if ( state == ZOO_CONNECTING_STATE )
			{
				context.apply_client_->OnConnecting();
			}
		}
	}

	void IZkApplyClient::ZkApplyClientImpl::OnConnected()
	{
		ZkAutoLock lock( &mutex_ );
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"OnConnected call\n");
		ZkSystemState old_state = system_state_;	
		system_state_ = zkConnected;
		LoadSource();

		if ( callback_ != NULL )
		{
			CallbackParam param;
			if ( old_state == zkReConnecting )
			{
				param.type = ReConnectCb;
			}
			else
			{			
				param.type = ConnectCb;
			}
			param.result = 0;
			param.context = callback_context_;
			callback_( &param );
		}
	}

	void IZkApplyClient::ZkApplyClientImpl::OnDisconnected()
	{
		ZkAutoLock lock( &mutex_ );
		Disconnect();

		if ( callback_ != NULL )
		{
			CallbackParam param;
			param.type = DisconnectCb;
			param.result = 0;
			param.context = callback_context_;
			callback_( &param );
		}
	}

	void IZkApplyClient::ZkApplyClientImpl::OnConnecting()
	{
		ZkAutoLock lock( &mutex_ );
		system_state_ = zkReConnecting;
		ZkClientPrint( ZK_LOG_LVL_KEYSTATUS," OnConnecting \n") ;
	}

	void IZkApplyClient::ZkApplyClientImpl::RemoveReserveNode( const char* path )
	{
		if ( path == NULL )
		{
			ReserveQueue::iterator itr = reserve_queue_.begin();
			while ( itr != reserve_queue_.end() )
			{
				NodeValue::Destory( itr->second );
				itr++;
			}
			reserve_queue_.clear();
		}
		else
		{
			ReserveQueue::iterator itr = reserve_queue_.find( path );
			if ( itr != reserve_queue_.end() )
			{
				NodeValue::Destory( itr->second );
				reserve_queue_.erase( itr );
			}
		}
	}

	void IZkApplyClient::ZkApplyClientImpl::RemoveSourceNode( const char* path )
	{
		if ( path == NULL )
		{
			Sources::iterator itr = sources_.begin();
			while ( itr != sources_.end() )
			{
				NodeValue::Destory( itr->second );
				itr++;
			}
			sources_.clear();
		}
		else
		{
			Sources::iterator itr = sources_.find( path );
			if ( itr != sources_.end() )
			{
				NodeValue::Destory( itr->second );
				sources_.erase( itr );
			}
		}
	}

	bool IZkApplyClient::ZkApplyClientImpl::Init( bool bInit /* = true */ )
	{
		bool bRet = false;
		if ( bInit == false )
		{
			bRet = pthread_mutex_destroy( &mutex_ );
		}
		else
		{
			pthread_mutexattr_t attr;
			pthread_mutexattr_init( &attr );	
			pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
			bRet = pthread_mutex_init( &mutex_, &attr );		
		}
		return bRet;
	}

	struct ThreadParam
	{
		zhandle_t* zkhandle_;
		string path_;
		unsigned auto_delete_time_;
	};

#ifdef WIN32
	unsigned  __stdcall AutoDeleteReserveNode(void *arg)
	{
		ThreadParam* param = (ThreadParam*)arg;
		if ( param != NULL )
		{
			string path = param->path_;
			zhandle_t* handle = param->zkhandle_;
#ifdef WIN32
			Sleep( param->auto_delete_time_ * 1000 );
#else
			sleep( param->auto_delete_time_ );
#endif
			// 尝试着删除，若handle已经被销毁，则删除失败
			try
			{
				zoo_adelete( handle, path.c_str(), -1, IZkApplyClient::ZkApplyClientImpl::VoidCB, NULL );	
			}
			catch (...)
			{
				ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"handle is null\n");
			}
			// 减少对zkhandle的引用计数
			
		}
		DEL_PTR(param);

		return 0;
	}
#endif

	void* AutoDeleteReserveNodeForLinux(void* buff)
	{
		ThreadParam* param = (ThreadParam*)buff;
		if ( param != NULL )
		{
			string path = param->path_;
			zhandle_t* handle = param->zkhandle_;
#ifdef WIN32
			Sleep( param->auto_delete_time_ * 1000 );
#else
			sleep( param->auto_delete_time_ );
#endif

			// 尝试着删除，若handle已经被销毁，则删除失败
			try
			{
				zoo_adelete( handle, path.c_str(), -1, IZkApplyClient::ZkApplyClientImpl::VoidCB, NULL );	
			}
			catch (...)
			{
				ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"AutoDeleteReserveNode Handle is NULL\n");
			}		
		}
		DEL_PTR(param);
		return 0;
	}


	void IZkApplyClient::ZkApplyClientImpl::ReserveNodeCreateCB(int rc, const char *value, const void *data)
	{
		ZkAutoLock lock( &IObjectContainer::mutex_ );	
		unsigned index = (unsigned int)data;
		Context context;
		if ( !Context::GetContext( index, context) )
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"ReserveNodeCreateCB context is null\n" );
		}

		if ( rc == ZOK )
		{
			ThreadParam* param = new ThreadParam();
			param->path_ = value;
			param->zkhandle_ = context.zkhanlde_;
			param->auto_delete_time_ = context.auto_delete_time_;

			ZkClientPrint( ZK_LOG_LVL_DETAIL,"auto delete reserve node time=%d path=%s\n", param->auto_delete_time_,value );

			if ( param->auto_delete_time_ != 0 )
			{
				pthread_t thd;
#ifdef WIN32
				pthread_create( &thd, 0, AutoDeleteReserveNode, param );	
#else
				pthread_create( &thd, NULL, AutoDeleteReserveNodeForLinux, param );	
#endif
				pthread_detach( thd );					
			}
			else
			{
				DEL_PTR(param);
			}
		}
		else
		{
			ZkClientPrint( ZK_LOG_LVL_KEYSTATUS,"create reserve node fail rc=%d\n", rc );
		}
		Context::Destory( index );
	}

	void IZkLogHelp::SetLog( bool is_open, int level )
	{
		// dll内部需要重定向io输出到新的控制台
#ifdef WIN32
		freopen("CONOUT$","w+t",stdout);          
		freopen("CONIN$","r+t",stdin);
#endif
		is_print_open = is_open;
	}
	void IZkLogHelp::SetSystemLog( bool is_open, int level )
	{
		zoo_set_debug_level( (ZooLogLevel)level );
	}
	void IZkLogHelp::SetPrintFunc(PrintFunc func)
	{
		Print = func;
	}
};