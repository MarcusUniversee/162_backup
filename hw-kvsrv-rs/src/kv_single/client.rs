use crate::client::*;
use crate::rpc::RpcClient;
use async_trait::async_trait;
use crate::kv_single::server::GetArgs;
use crate::kv_single::server::PutArgs;
use crate::kv_single::server::GetReply;
use crate::kv_single::server::PutReply;
use std::time::Duration;

pub struct Client {
    endpoint: RpcClient,
}

impl Client {
    pub fn new(endpoint: RpcClient) -> Self {
        Client { endpoint }
    }
}

#[async_trait]
impl KvClient for Client {
    async fn get(&self, key: &str) -> Result<(String, Version), KVError> {
        // TODO: call get and decode the reply.
        let args = GetArgs {
            key: key.to_string(),
        };
        let reply_wrapped = self.endpoint.call("get", &args).await; //first try
        if !reply_wrapped.is_none() {
            let reply: GetReply = reply_wrapped.unwrap();
            match reply.err {
                Ok(()) => {return Ok((reply.value, reply.version))},
                Err(e) => {return Err(e)},
            }
        }

        loop {
            //retries
            let reply_wrapped = self.endpoint.call("get", &args).await;
            if reply_wrapped.is_none() {
                tokio::time::sleep(Duration::from_millis(10)).await;
            } else {
                let reply: GetReply = reply_wrapped.unwrap();
                match reply.err {
                    Ok(()) => {return Ok((reply.value, reply.version))},
                    Err(e) => {
                        if e == KVError::Version {
                            return Err(KVError::Maybe);
                        }
                        return Err(e);
                    },  
                }
            }
        }
    }

    async fn put(&self, key: &str, value: &str, version: Version) -> Result<(), KVError> {
        let args = PutArgs {
            key: key.to_string(),
            value: value.to_string(),
            version: version,
        };
        let reply_wrapped = self.endpoint.call("put", &args).await;
        if !reply_wrapped.is_none() {
            let reply: PutReply = reply_wrapped.unwrap();
            match reply.err {
                Ok(()) => {return Ok(())},
                Err(e) => {return Err(e)},
            }
        }
        loop {
            //retries
            let reply_wrapped = self.endpoint.call("put", &args).await;
            if reply_wrapped.is_none() {
                tokio::time::sleep(Duration::from_millis(10)).await;
            } else {
                let reply: PutReply = reply_wrapped.unwrap();
                match reply.err {
                    Ok(()) => {return Ok(())},
                    Err(e) => {
                        if e == KVError::Version {
                            return Err(KVError::Maybe);
                        }
                        return Err(e);
                    },  
                }
            }
        }
    }
}
