/**
 * Copyright (C) 2013-2016 ECNU_DaSE.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * @file ob_session_mgr.h
 * @brief BaseSessionCtx
 *     modify by guojinwei, bingo: support REPEATABLE-READ isolation
 *     add transaction information
 *
 * @version CEDAR 0.2
 * @author guojinwei <guojinwei@stu.ecnu.edu.cn>
 *         bingo <bingxiao@stu.ecnu.edu.cn>
 * @date 2016_06_16
 */

////===================================================================
 //
 // ob_session_mgr.h updateserver / Oceanbase
 //
 // Copyright (C) 2010 Taobao.com, Inc.
 //
 // Created on 2012-08-19 by Yubai (yubai.lk@taobao.com)
 //
 // -------------------------------------------------------------------
 //
 // Description
 //
 //
 // -------------------------------------------------------------------
 //
 // Change Log
 //
////====================================================================

#ifndef  OCEANBASE_UPDATESERVER_SESSION_MGR_H_
#define  OCEANBASE_UPDATESERVER_SESSION_MGR_H_
#include "common/ob_define.h"
#include "common/page_arena.h"
#include "common/ob_fixed_queue.h"
#include "common/ob_list.h"
#include "common/ob_trace_log.h"
#include "common/ob_timer.h"
#include "common/ob_new_scanner.h"
#include "common/ob_transaction.h"
#include "common/ob_id_map.h"
#include "common/utility.h"
#include "common/ob_spin_lock.h"
#include "common/priority_packet_queue_thread.h"
#include "ob_ups_mutator.h"
#include "ob_inc_seq.h"
//add hushuang[scalablecommit]20160415
#include "common/ob_commit_queue.h"
//#include "ob_session_mgr.h"
//add e


#define INVALID_SESSION_DESCRIPTOR IDMAP_INVALID_ID

//add hushuang[scalablecommit]20160415
#define SetTxCAS(x, old_val, new_val) __sync_bool_compare_and_swap(x, old_val, new_val)
//add e

namespace oceanbase
{
  namespace updateserver
  {
    class BaseSessionCtx;
    class ObTriggerHandler;
    class LockMgr;
    class ISessionCallback
    {
      public:
        virtual ~ISessionCallback() {};
      public:
        virtual int cb_func(const bool rollback, void *data, BaseSessionCtx &session) = 0;
    };

    enum SessionType
    {
      SESSION_TYPE_START = -1,
      ST_READ_ONLY = 0,
      ST_REPLAY = 1,
      ST_READ_WRITE = 2,
      SESSION_TYPE_NUM,
    };

    class SessionMgr;
    class ISessionCtxFactory
    {
      public:
        virtual ~ISessionCtxFactory() {};
      public:
        virtual BaseSessionCtx *alloc(const SessionType type, SessionMgr &host) = 0;
        virtual void free(BaseSessionCtx *ptr) = 0;
    };

    class SessionTableLockInfo;
    class BaseSessionCtx
    {
      public:
        enum ES_STAT
        {
          ES_NONE = 0,
          ES_ALL = ~0,
          ES_CALLBACK = 1,
          ES_UPDATE_COMMITED_TRANS_ID = 2,
          ES_PUBLISH = 4,
          ES_FREE = 8,
        };
      public:
        BaseSessionCtx(const SessionType type,
                       SessionMgr &host) : type_(type),
                                           host_(host)

        {
          reset();
        };
        virtual ~BaseSessionCtx() {};
      public:
        bool need_to_do(ES_STAT flag)
        {
          return es_stat_ & flag;
        }
        void mark_done(ES_STAT flag)
        {
          es_stat_ &= ~flag;
        }
        virtual void end(const bool need_rollback) // on commit
        {
          UNUSED(need_rollback);
        };
        virtual void publish() // on publish
        {
          // do nothing
        };
        virtual void on_free() // on free
        {
          // do nothing
        };
        virtual void reset()
        {
          trans_id_ = INT64_MAX;
          start_handle_time_ = 0;
          session_start_time_ = 0;
          stmt_start_time_ = 0;
          session_timeout_ = INT64_MAX;
          stmt_timeout_ = INT64_MAX;
          session_descriptor_ = UINT32_MAX;
          es_stat_ = ES_ALL;
          self_processor_index_ = 0;
          conflict_processor_index_ = 0;
          CLEAR_TRACE_BUF(tlog_buffer_);
          priority_ = common::PriorityPacketQueueThread::NORMAL_PRIV;
          last_proc_time_ = 0;
          // add by guojinwei [repeatable read] 20160417:b
          trans_start_time_ = INT64_MAX;
          trans_descriptor_ = UINT32_MAX;
          // add:e
        };
        virtual void kill()
        {
          // do nothing;
        };
        virtual bool is_killed() const
        {
          return false;
        };
        virtual int init_lock_info(LockMgr &lock_mgr, const common::IsolationLevel isolation)
        {
          UNUSED(lock_mgr);
          UNUSED(isolation);
          return common::OB_SUCCESS;
        };
        //add wangjiahao [tablelock] 20160616 :b
        virtual int init_table_lock_info()
        {
          return common::OB_SUCCESS;
        }

        virtual SessionTableLockInfo* get_tblock_info()
        {
          return NULL;
        }
        //add :e
        virtual int add_publish_callback(ISessionCallback *callback, void *data)
        {
          UNUSED(callback);
          UNUSED(data);
          return common::OB_SUCCESS;
        };
        virtual int add_free_callback(ISessionCallback *callback, void *data)
        {
          UNUSED(callback);
          UNUSED(data);
          return common::OB_SUCCESS;
        };
      public:
        SessionType get_type() const
        {
          return type_;
        };

        virtual void set_frozen(){}

        virtual bool is_frozen() const {return false;}

        int64_t get_trans_id() const
        {
          return trans_id_;
        };
        void set_trans_id(const int64_t trans_id)
        {
          trans_id_ = trans_id;
        };

        void set_start_handle_time(const int64_t time)
        {
          start_handle_time_ = time;
        }
        int64_t get_start_handle_time() const
        {
          return start_handle_time_;
        }
        int64_t get_session_start_time() const
        {
          return session_start_time_;
        };
        void set_session_start_time(const int64_t session_start_time)
        {
          session_start_time_ = session_start_time;
          last_active_time_ = session_start_time;;
        };
        int64_t get_session_timeu() const
        {
          return tbsys::CTimeUtil::getTime() - session_start_time_;
        };

        int64_t get_stmt_start_time() const
        {
          return stmt_start_time_;
        };
        void set_stmt_start_time(const int64_t stmt_start_time)
        {
          stmt_start_time_ = stmt_start_time;
          last_active_time_ = stmt_start_time;;
        };

        int64_t get_session_timeout() const
        {
          return session_timeout_;
        };
        void set_session_timeout(const int64_t timeout)
        {
          session_timeout_ = timeout;
        };

        bool is_session_expired() const
        {
          int64_t cur_time = tbsys::CTimeUtil::getTime();
          bool bret = ((cur_time - session_start_time_) > session_timeout_
                      || (cur_time - last_active_time_) > session_idle_time_);
          if (bret)
          {
            TBSYS_LOG(WARN, "session expired, sd=%u ctx=%p cur_time=%ld "
                      "session_start_time=%ld session_timeout=%ld "
                      "last_active_time=%ld session_idle_time=%ld",
                      session_descriptor_, this, cur_time,
                      session_start_time_, session_timeout_,
                      last_active_time_, session_idle_time_);
          }
          return bret;
        };

        int64_t get_stmt_timeout() const
        {
          return stmt_timeout_;
        };
        void set_stmt_timeout(const int64_t timeout)
        {
          stmt_timeout_ = timeout;
        };
        bool is_stmt_expired() const
        {
          int64_t cur_time = tbsys::CTimeUtil::getTime();
          bool bret = (cur_time - stmt_start_time_) > stmt_timeout_;
          return bret;
        };

        int64_t get_session_idle_time() const
        {
          return session_idle_time_;
        };
        void set_session_idle_time(const int64_t time)
        {
          session_idle_time_ = time;
        };

        int64_t get_last_active_time() const
        {
          return last_active_time_;
        };
        void set_last_active_time(const int64_t time)
        {
          last_active_time_ = time;
        };

        uint32_t get_session_descriptor() const
        {
          return session_descriptor_;
        };
        void set_session_descriptor(const uint32_t session_descriptor)
        {
          session_descriptor_ = session_descriptor;
        };

        SessionMgr &get_host()
        {
          return host_;
        };
        common::TraceLog::LogBuffer& get_tlog_buffer() const
        {
          return tlog_buffer_;
        };

        int64_t to_string(char* buf, int64_t len) const
        {
          int64_t pos = 0;
          // modify by guojinwei [repeatable read] 20160417:b
          //common::databuff_printf(buf, len, pos, "Session type=%d trans_id=%ld start_time=%ld timeout=%ld stmt_start_time=%ld stmt_timeout=%ld idle_time=%ld last_active_time=%ld descriptor=%d",
          //                type_, trans_id_, session_start_time_, session_timeout_, stmt_start_time_, stmt_timeout_, session_idle_time_, last_active_time_, session_descriptor_);
          common::databuff_printf(buf, len, pos, "Session type=%d trans_id=%ld start_time=%ld "
                                  "timeout=%ld stmt_start_time=%ld stmt_timeout=%ld idle_time=%ld "
                                  "last_active_time=%ld descriptor=%d"
                                  "trans_start_time=%ld trans_descriptor=%d",
                                  type_, trans_id_, session_start_time_,
                                  session_timeout_, stmt_start_time_, stmt_timeout_, session_idle_time_,
                                  last_active_time_, session_descriptor_,
                                  trans_start_time_, trans_descriptor_);
          // modify:e
          return pos;
        }

        void lock_session()
        {
          session_lock_.lock();
        };
        void unlock_session()
        {
          session_lock_.unlock();
        };

        void set_self_processor_index(const int64_t processor_index)
        {
          self_processor_index_ = processor_index;
        };
        int64_t get_self_processor_index() const
        {
          return self_processor_index_;
        };

        void set_conflict_processor_index(const int64_t processor_index)
        {
          conflict_processor_index_ = processor_index;
        };
        int64_t get_conflict_processor_index() const
        {
          return conflict_processor_index_;
        };

        void set_priority(common::PriorityPacketQueueThread::QueuePriority priority)
        {
          priority_ = priority;
        };
        common::PriorityPacketQueueThread::QueuePriority get_priority() const
        {
          return priority_;
        };

        void set_last_proc_time(const int64_t proc_time)
        {
          last_proc_time_ = proc_time;
        };
        int64_t get_last_proc_time() const
        {
          return last_proc_time_;
        };
        // add by guojinwei [repeatable read] 20160417:b
        void set_trans_start_time(const int64_t trans_start_time)
        {
          trans_start_time_ = trans_start_time;
        };
        int64_t get_trans_start_time() const
        {
          return trans_start_time_;
        };
        uint32_t get_trans_descriptor() const
        {
          return trans_descriptor_;
        };
        void set_trans_descriptor(const uint32_t trans_descriptor)
        {
          trans_descriptor_ = trans_descriptor;
        };
        // add:e

      private:
        const SessionType type_;
        SessionMgr &host_;
        int64_t trans_id_;
        int64_t start_handle_time_;
        int64_t session_start_time_;
        int64_t stmt_start_time_;
        int64_t session_timeout_;
        int64_t stmt_timeout_;
        int64_t session_idle_time_;
        int64_t last_active_time_;
        uint32_t session_descriptor_;
        int32_t es_stat_; // end session state
        mutable common::TraceLog::LogBuffer tlog_buffer_;
        common::ObSpinLock session_lock_;
        int64_t self_processor_index_;
        int64_t conflict_processor_index_;
        common::PriorityPacketQueueThread::QueuePriority priority_;
        int64_t last_proc_time_;
        // add by guojinwei [repeatable read] 20160417:b
        int64_t trans_start_time_;    ///< only for repeatable read
        uint32_t trans_descriptor_;   ///< only for repeatable read
        // add:e
    };

    class CallbackMgr
    {
      struct CallbackInfo
      {
        ISessionCallback *callback;
        void *data;
        CallbackInfo *next;
      };
      public:
        CallbackMgr();
        ~CallbackMgr();
      public:
        template <typename Allocator>
        int add_callback_info(Allocator &allocator, ISessionCallback *callback, void *data);
        template <typename Allocator>
        int prepare_callback_info(Allocator &allocator, ISessionCallback *callback, void *data);
        void commit_prepare_list();
        void rollback_prepare_list(BaseSessionCtx &session);
        void reset();
        int callback(const bool rollback, BaseSessionCtx &session);
      private:
        CallbackInfo *callback_list_;
        CallbackInfo *prepare_list_;
    };

    class SessionMgr;
    class MinTransIDGetter
    {
      public:
        MinTransIDGetter(SessionMgr &sm);
        ~MinTransIDGetter();
        void operator()(const uint32_t sd);
        int64_t get_min_trans_id();
      private:
        SessionMgr &sm_;
        int64_t min_trans_id_;
    };

    class ZombieKiller
    {
      public:
        ZombieKiller(SessionMgr &sm, const bool force);
        ~ZombieKiller();
        void operator()(const uint32_t sd);
      private:
        SessionMgr &sm_;
        const bool force_;
    };

    class ShowSessions
    {
      public:
        static const uint64_t SESSION_TABLE_ID = 100;
        static const uint64_t SESSION_COL_ID_TYPE = 16;   // type
        static const uint64_t SESSION_COL_ID_SD = 17;     // session descriptor
        static const uint64_t SESSION_COL_ID_TID = 18;    // trans id
        static const uint64_t SESSION_COL_ID_STIME = 19;  // session start time
        static const uint64_t SESSION_COL_ID_SSTIME = 20; // stmt start time
        static const uint64_t SESSION_COL_ID_TIMEO = 21;  // session timeout
        static const uint64_t SESSION_COL_ID_STIMEO = 22; // stmt timeout
        static const uint64_t SESSION_COL_ID_ITIME = 23; // session idle time
        static const uint64_t SESSION_COL_ID_ATIME = 24; // session last active time
      public:
        ShowSessions(SessionMgr &sm, common::ObNewScanner &scanner);
        ~ShowSessions();
        void operator()(const uint32_t sd);
        common::ObNewScanner &get_scanner();
        const common::ObRowDesc &get_row_desc() const;
      private:
        SessionMgr &sm_;
        common::ObNewScanner &scanner_;
        common::ObRowDesc row_desc_;
        common::ObRow row_;
    };

    class SessionLockGuard
    {
      public:
        SessionLockGuard(const SessionType type,
                        common::SpinRWLock &lock) : lock_(NULL),
                                                    lock_succ_(false)
        {
          if (ST_READ_WRITE == type)
          {
            if (lock.try_rdlock())
            {
              lock_ = &lock;
              lock_succ_ = true;
            }
          }
          else
          {
            lock_succ_ = true;
          }
        };
        ~SessionLockGuard()
        {
          if (NULL != lock_)
          {
            lock_->unlock();
          }
        };
        bool is_lock_succ() const
        {
          return lock_succ_;
        };
      private:
        common::SpinRWLock *lock_;
        bool lock_succ_;
    };

    //mod by hushuang[scalablecommit] 20160415
    //class SessionMgr : public tbsys::CDefaultRunnable
    class SessionMgr : public common::ISessionMgr, public tbsys::CDefaultRunnable
    //mod e
    {
      typedef common::ObFixedQueue<BaseSessionCtx> SessionCtxList;
      typedef common::ObIDMap<BaseSessionCtx, uint32_t> SessionCtxMap;
      public:
        static const int64_t CALC_INTERVAL = 2000;
        static const int64_t FORCE_KILL_WAITTIME = 100000;

      public:
        SessionMgr();
        ~SessionMgr();

      public:
        int init(const uint32_t max_ro_num,
                const uint32_t max_rp_num,
                const uint32_t max_rw_num,
                ISessionCtxFactory *factory);

        void destroy();

        virtual void run(tbsys::CThread* thread, void* arg);

      public:
        int begin_session(const SessionType type, const int64_t start_time, const int64_t timeout, const int64_t idle_time, uint32_t &session_descriptor);
        int precommit(const uint32_t session_descriptor);
		//modify by zhouhuan [scalablecommit]
        //int end_session(const uint32_t session_descriptor, const bool rollback = true, const bool force = true,
        //                const uint64_t es_flag = (uint64_t)BaseSessionCtx::ES_ALL);
		int end_session(const uint32_t session_descriptor, const bool rollback = true, const bool force = true,
        const uint64_t es_flag = (uint64_t)BaseSessionCtx::ES_ALL, const bool is_slave = false);
        int update_commited_trans_id(BaseSessionCtx* ctx);

        template <class CTX>
        CTX *fetch_ctx(const uint32_t session_descriptor);
        BaseSessionCtx *fetch_ctx(const uint32_t session_descriptor);
        void revert_ctx(const uint32_t session_descriptor);
        int64_t get_processor_index(const uint32_t session_descriptor);

        int64_t get_min_flying_trans_id();
        void flush_min_flying_trans_id();
        int wait_write_session_end_and_lock(const int64_t timeout_us);
        void unlock_write_session();
        void kill_zombie_session(const bool force);
        int kill_session(const uint32_t session_descriptor);
        void show_sessions(common::ObNewScanner &scanner);
        int64_t get_commited_trans_id() const;
        int64_t get_published_trans_id() const;
        int64_t get_flying_rosession_num() const {return ctx_list_[ST_READ_ONLY].get_free();};
        int64_t get_flying_rpsession_num() const {return ctx_list_[ST_REPLAY].get_free();};
        int64_t get_flying_rwsession_num() const {return ctx_list_[ST_READ_WRITE].get_free();};
        IncSeq& get_trans_seq() { return trans_seq_; }
        void enable_start_write_session(){ allow_start_write_session_ = true; }
        void disable_start_write_session(){ allow_start_write_session_ = false; }
        //add hushuang[scalablecommit]20160415
        virtual int update_published_trans_id(int64_t trans_id);///< used for update_published_trans_id of scalable commit
        virtual int update_commited_trans_id(int64_t trans_id);///< used for update_commited_trans_id of scalable commit
        //virtual int get_group_id(uint32_t session_descriptor, int64_t& group_id);
        //add e
       //add chujiajia [log synchronization][multi_cluster] 20160606:b
        inline int reset(int32_t max_ro_num, int32_t max_rp_num, int32_t max_rw_num)
        {
          int ret = common::OB_SUCCESS;
          ctx_map_.destroy();
          for (int i = 0; i < SESSION_TYPE_NUM; i++)
          {
            BaseSessionCtx *ctx = NULL;
            while (common::OB_SUCCESS == ctx_list_[i].pop(ctx))
            {
              if (NULL != ctx
                  && NULL != factory_)
              {
                factory_->free(ctx);
              }
            }
            ctx_list_[i].destroy();
          }
          if (common::OB_SUCCESS != (ret = ctx_map_.init(max_ro_num + max_rp_num + max_rw_num)))
          {
            TBSYS_LOG(WARN, "init ctx_map fail, ret=%d num=%u", ret, max_ro_num + max_rp_num + max_rw_num);
          }
          else
          {
            const int64_t MAX_CTX_NUM[SESSION_TYPE_NUM] = {max_ro_num, max_rp_num, max_rw_num};
            BaseSessionCtx *ctx = NULL;
            for (int i = 0; i < SESSION_TYPE_NUM; i++)
            {
              if (common::OB_SUCCESS != (ret = ctx_list_[i].init(MAX_CTX_NUM[i])))
              {
                TBSYS_LOG(WARN, "init ctx_list fail, ret=%d type=%d max_ctx_num=%ld", ret, i, MAX_CTX_NUM[i]);
                break;
              }
              for (int64_t j = 0; common::OB_SUCCESS == ret && j < MAX_CTX_NUM[i]; j++)
              {
                if (NULL == (ctx = factory_->alloc((SessionType)i, *this)))
                {
                  TBSYS_LOG(WARN, "alloc ctx fail, type=%d", i);
                  ret = common::OB_MEM_OVERFLOW;
                  break;
                }
                if (common::OB_SUCCESS != (ret = ctx_list_[i].push(ctx)))
                {
                  TBSYS_LOG(WARN, "push ctx to list fail, ret=%d ctx=%p", ret, ctx);
                  break;
                }
              }
            }
          }
          inited_ = true;
          return ret;
        }
        //add:e
      private:
        BaseSessionCtx *alloc_ctx_(const SessionType type);
        void free_ctx_(BaseSessionCtx *ctx);

      private:
        bool inited_;
        ISessionCtxFactory *factory_ CACHE_ALIGNED;
        IncSeq trans_seq_ CACHE_ALIGNED;
        volatile int64_t published_trans_id_ CACHE_ALIGNED;
        volatile int64_t commited_trans_id_ CACHE_ALIGNED;
        mutable volatile int64_t min_flying_trans_id_ CACHE_ALIGNED;
        mutable volatile int64_t calc_timestamp_;

        SessionCtxList ctx_list_[SESSION_TYPE_NUM];
        SessionCtxMap ctx_map_;

        common::SpinRWLock session_lock_;
        bool allow_start_write_session_;
    };

    template <class CTX>
    CTX *SessionMgr::fetch_ctx(const uint32_t session_descriptor)
    {
      CTX *ret = NULL;
      BaseSessionCtx *ctx = fetch_ctx(session_descriptor);
      if (NULL != ctx)
      {
        ret = dynamic_cast<CTX*>(ctx);
        if (NULL == ret)
        {
          revert_ctx(session_descriptor);
        }
      }
      return ret;
    }

    template <typename Allocator>
    int CallbackMgr::add_callback_info(Allocator &allocator, ISessionCallback *callback, void *data)
    {
      int ret = common::OB_SUCCESS;
      CallbackInfo *cbi = NULL;
      if (NULL == callback)
      {
        ret = common::OB_INVALID_ARGUMENT;
      }
      else if (NULL == (cbi = (CallbackInfo*)allocator.alloc(sizeof(CallbackInfo))))
      {
        ret = common::OB_MEM_OVERFLOW;
      }
      else
      {
        cbi->callback = callback;
        cbi->data = data;
        cbi->next = callback_list_;
        callback_list_ = cbi;
      }
      return ret;
    }

    template <typename Allocator>
    int CallbackMgr::prepare_callback_info(Allocator &allocator, ISessionCallback *callback, void *data)
    {
      int ret = common::OB_SUCCESS;
      CallbackInfo *cbi = NULL;
      if (NULL == callback)
      {
        ret = common::OB_INVALID_ARGUMENT;
      }
      else if (NULL == (cbi = (CallbackInfo*)allocator.alloc(sizeof(CallbackInfo))))
      {
        ret = common::OB_MEM_OVERFLOW;
      }
      else
      {
        cbi->callback = callback;
        cbi->data = data;
        cbi->next = prepare_list_;
        prepare_list_ = cbi;
      }
      return ret;
    }

    class KillZombieDuty : public common::ObTimerTask
    {
      public:
        static const int64_t SCHEDULE_PERIOD = 1L * 1000000L;
      public:
        KillZombieDuty() {};
        virtual ~KillZombieDuty() {};
        virtual void runTimerTask();
    };
  }
}

#endif //OCEANBASE_UPDATESERVER_SESSION_MGR_H_

