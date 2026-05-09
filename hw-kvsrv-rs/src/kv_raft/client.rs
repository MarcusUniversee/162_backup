use crate::client::*;
use crate::rpc::RpcClient;
use async_trait::async_trait;
use crate::kv_raft::rsm::{GetReply, PutReply, ReplyErr};
use crate::kv_single::server::{GetArgs, PutArgs};
use std::sync::atomic::{AtomicUsize, Ordering};
//Source: https://doc.rust-lang.org/std/sync/atomic/struct.AtomicUsize.html
use std::sync::Arc;
use std::time::Duration;

pub struct Client {
    servers: Vec<Arc<RpcClient>>,
    // TODO: cache leader and client state.
    leader: AtomicUsize, //Remember which server responded successfully last time and try it first
}

impl Client {
    pub fn new(servers: Vec<Arc<RpcClient>>) -> Self {
        Self {
            servers,
            leader: AtomicUsize::new(0),
        }
    }
}

#[async_trait]
impl KvClient for Client {
    //helper function for both put and get that retries, then goes to the next server
    async fn get(&self, key: &str) -> Result<(String, Version), KVError> {
        let args = GetArgs {
            key: key.to_string(),
        };
        //loop to recycle through servers even after we made a full loop
        let mut retry = false;
        loop {
            //now start trying the different servers
            let cur_server = self.leader.load(Ordering::Relaxed);
            for i_ in 0..self.servers.len() {
                //iterate through all the possible servers starting at the leader
                let i = (cur_server + i_) % self.servers.len();
                //use clone instead of &args because we already did &args when we called it
                let reply_wrapped = self.servers[i].call("get", &args).await; //first try
                if !reply_wrapped.is_none() {
                    let reply: GetReply = reply_wrapped.unwrap();
                    match reply.err {
                        Ok(kv_err) => { //if no wrong leader, check the internal kv error
                            //e could be a kv error, but at least server ran succesfully
                            self.leader.store(i, Ordering::Relaxed);
                            match kv_err {
                                Ok(()) => {
                                    return Ok((reply.value, reply.version));
                                }
                                Err(KVError::Maybe) => {
                                    continue;
                                }
                                Err(e) => {
                                    return Err(e);
                                }
                            }
                        }
                        Err(e) => {
                            continue //wrong leader error
                        }
                    }
                }
            }
            retry = true;
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
    }

    async fn put(&self, key: &str, value: &str, version: Version) -> Result<(), KVError> {
        let args = PutArgs {
            key: key.to_string(),
            value: value.to_string(),
            version: version,
        };
        //loop to recycle through servers even after we made a full loop
        let mut retry = false;
        loop {
            //now start trying the different servers
            let cur_server = self.leader.load(Ordering::Relaxed);
            for i_ in 0..self.servers.len() {
                //iterate through all the possible servers starting at the leader
                let i = (cur_server + i_) % self.servers.len();
                //use clone instead of &args because we already did &args when we called it
                let reply_wrapped = self.servers[i].call("put", &args).await; //first try
                if !reply_wrapped.is_none() {
                    let reply: PutReply = reply_wrapped.unwrap();
                    match reply.err {
                        Ok(kv_err) => { //if no wrong leader, check the internal kv error
                            //e could be a kv error, but at least server ran succesfully
                            self.leader.store(i, Ordering::Relaxed);
                            match kv_err {
                                Ok(()) => {
                                    return Ok(());
                                }
                                Err(e) => {
                                    if e == KVError::Version && retry {
                                        //maybe
                                        match self.get(key).await {
                                            Ok((val, vers)) => {
                                                if val == value && vers == version + 1 {
                                                    //success
                                                    return Ok(());
                                                }
                                                if vers != version {
                                                    //failure to due versioning
                                                    return Err(KVError::Version);
                                                }
                                                //else retry
                                                continue;
                                            }
                                            Err(e) => {
                                                if e == KVError::NoKey && version == 0 {
                                                    continue; //was not added as a new key, try again
                                                }
                                                return Err(e);
                                            }
                                        }
                                    }
                                    return Err(e);
                                }
                            }
                        }
                        Err(e) => {
                            continue //wrong leader error
                        }
                    }
                }
            }
            retry = true;
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
    }
}
