use crate::kv_raft::raft::Raft;
use crate::client::*;
use crate::rpc::*;
use async_trait::async_trait;
use std::sync::Arc;
use crate::kv_single::server::{GetArgs, PutArgs, KVServer};
use serde::{Deserialize, Serialize};
use thiserror::Error;
use tokio::sync::Mutex;
use std::time::Duration;


pub type Version = u64;

//copy of KVError
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize, Error)]
pub enum ReplyErr {
    #[error("ErrWrongLeader")]
    WrongLeader,
    #[error("Maybe")]
    Maybe,
}


#[derive(Serialize, Deserialize)]
pub struct GetReply {
    pub value: String,
    pub version: Version,
    pub err: Result<Result<(), KVError>, ReplyErr>,
}

#[derive(Serialize, Deserialize)]
pub struct PutReply {
    pub err: Result<Result<(), KVError>, ReplyErr>,
}

struct Entry {
    version: Version,
    value: String,
}

pub struct RSM {
    raft: Arc<Raft>,
    kv: KVServer,
    latest_apply: Mutex<usize>, 
    
}

impl RSM {
    pub fn new(raft: Arc<Raft>) -> Arc<Self> {
        Arc::new(Self {
            raft: raft,
            kv: KVServer::new(),
            latest_apply: Mutex::new(0),
        })
    }

    pub fn raft(&self) -> &Arc<Raft> {
        &self.raft
    }

    //for operations that were not made on this raft server (but made on others)
    //their commits were replicated on this raft server's log, so we need to apply
    //so this syncs changes up to a specified commit, since if we get
    //at that commit, we only care about everything up to that point
    async fn sync_puts_with_commits(&self, idx: usize) {
        let mut latest_apply = self.latest_apply.lock().await;
        loop {
            let next_apply = *latest_apply + 1;
            if next_apply > idx {
                break; //only apply changes before idx
            }
            match self.raft.get_committed(next_apply).await {
                Some(cmd) => {
                    if let Ok(args) = serde_json::from_str::<PutArgs>(&cmd) {
                        let kv_serv = self.kv.put(&args).await;
                    }
                    *latest_apply = next_apply;
                }
                None => {
                    break;
                }
            }
        }
    }

    //submits a command to raft and then waits for commit
    async fn raft_submit_commit(&self, cmd: String) -> Result<usize, ReplyErr> {
        let (idx, term) = match self.raft.submit(&cmd).await {
            Some(repl) => repl,
            None => return Err(ReplyErr::WrongLeader),
        };
        let mut count = 0;
        loop {
            //1000 ms is our timeout to give time for potential leader elections
            if count >= 1000 {
                return Err(ReplyErr::Maybe);
            }
            match self.raft.get_committed(idx).await {
                Some(cmd_) => {
                    if cmd_ != cmd  {
                        return Err(ReplyErr::WrongLeader);
                    } else {
                        return Ok(idx);
                    }
                }
                None => {
                    //not committed yet
                    }
            }

            tokio::time::sleep(Duration::from_millis(10)).await;
            count += 10;
        }
    }

    async fn put(&self, args: &PutArgs) -> PutReply {
        let str_cmd = serde_json::to_string(&args).unwrap();
        match self.raft_submit_commit(str_cmd).await {
            Ok(idx) => {
                self.sync_puts_with_commits(idx).await;
                let get_args = GetArgs {
                    key: args.key.clone()
                };
                let verify = self.kv.get(&get_args).await;
                if verify.value == args.value && verify.version == args.version + 1 {
                    return PutReply {
                        err: Ok(Ok(())),
                    }
                }
                return PutReply {
                    err: Ok(Err(KVError::Version)),
                }
            }
            Err(e) => {
                if e == ReplyErr::WrongLeader {
                    return PutReply {
                        err: Err(ReplyErr::WrongLeader),
                    }
                } else {
                    return PutReply {
                        err: Ok(Err(KVError::Maybe)),
                    }
                }
            }
        }
        
    }
    async fn get(&self, args: &GetArgs) -> GetReply {
        let str_cmd = serde_json::to_string(&args).unwrap();
        match self.raft_submit_commit(str_cmd).await {
            Ok(idx) => {
                self.sync_puts_with_commits(idx).await;
                let kv_serv = self.kv.get(args).await;
                return GetReply {
                    value: kv_serv.value,
                    version: kv_serv.version,
                    err: Ok(kv_serv.err),
                }
            }
            Err(e) => {
                if e == ReplyErr::WrongLeader {
                    return GetReply {
                        value: String::new(),
                        version: 0,
                        err: Err(ReplyErr::WrongLeader),
                    }
                } else {
                    return GetReply {
                        value: String::new(),
                        version: 0,
                        err: Ok(Err(KVError::Maybe)),
                    }
                }
            }
        }
    }
}

#[async_trait]
impl Server for RSM {
    async fn dispatch(&self, name: &str, args: String) -> String {
        match name {
            "get" => {
                serde_json::to_string(&self.get(&serde_json::from_str(&args).unwrap()).await).unwrap()
            }
            "put" => {
                serde_json::to_string(&self.put(&serde_json::from_str(&args).unwrap()).await).unwrap()
            }
            _ => self.raft.dispatch(name, args).await,
        }
    }
}
