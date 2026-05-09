use crate::client::*;
use crate::rpc::*;
use tokio::sync::RwLock;
use std::collections::HashMap;
use serde::{Deserialize, Serialize};

// TODO: define RPC request/reply types.

#[derive(Serialize, Deserialize)]
pub struct GetArgs {
    pub key: String
}

#[derive(Serialize, Deserialize)]
pub struct GetReply {
    pub value: String,
    pub version: Version,
    pub err: Result<(), KVError>,
}

#[derive(Serialize, Deserialize)]
pub struct PutArgs {
    pub key: String,
    pub value: String,
    pub version: Version,
}

#[derive(Serialize, Deserialize)]
pub struct PutReply {
    pub err: Result<(), KVError>,
}

struct Entry {
    version: Version,
    value: String,
}

pub struct KVServer {
    // TODO: add KV map and synchronization.
    pub data: RwLock<HashMap<String, Entry>>,
}

impl KVServer {
    pub fn new() -> Self {
        //init the kvserver struct
        Self {
            data: RwLock::new(HashMap::new()),
        }
    }

    pub async fn get(&self, args: &GetArgs) -> GetReply {
        let map = self.data.read().await;
        match map.get(&args.key) {
            Some(entry) => GetReply {
                value: entry.value.clone(),
                version: entry.version,
                err: Ok(()),
            },
            None => GetReply {
                //populate with something empty?
                value: String::new(),
                version: 0,
                err: Err(KVError::NoKey),
            }
        }
    }

    pub async fn put(&self, args: &PutArgs) -> PutReply {
        let mut map = self.data.write().await;
        match map.get(&args.key) {
            Some(entry) => {
                if args.version != entry.version {
                    return PutReply {
                        err: Err(KVError::Version),
                    }
                }
                map.insert(args.key.clone(), Entry {
                    version: args.version + 1,
                    value: args.value.clone(),
                });
                return PutReply {
                    err: Ok(()),
                };
            },
            None => {
                if args.version != 0 {
                    return PutReply {
                        err: Err(KVError::NoKey),
                    };
                }
                map.insert(args.key.clone(), Entry {
                    version: args.version + 1,
                    value: args.value.clone(),
                });
                return PutReply {
                    err: Ok(()),
                };
            }
        }
    }
}

#[async_trait::async_trait]
impl Server for KVServer {
    async fn dispatch(&self, name: &str, args: String) -> String {
        //had to add await since I made get and put async to use the RWLock
        match name {
            "get" => {
                serde_json::to_string(&self.get(&serde_json::from_str(&args).unwrap()).await).unwrap()
            }
            "put" => {
                serde_json::to_string(&self.put(&serde_json::from_str(&args).unwrap()).await).unwrap()
            }
            _ => panic!("unknown request name"),
        }
    }
}
