// Part 2: Distributed Lock with KV Server
// Part 4: Distributed Lock with Raft

use std::sync::Arc;

use crate::client::*;
use std::time::Duration;
use rand::prelude::*;

pub struct Lock {
    client: Arc<dyn KvClient>,
    lockname: String,
    // TODO: add any other lock client state.
    token: u64, //the random unique identifier
}

impl Lock {
    pub fn new(client: Arc<dyn KvClient>, lockname: &str) -> Self {
        // TODO: initialize lock state.
        let mut rng = rand::rng();
        Self {
            client: client,
            lockname: lockname.to_string(),
            token: rng.random::<u64>(),
        }
    }

    pub async fn acquire(&mut self) {
        // TODO: acquire the lock through the KV client.
        loop {
            match self.client.get(&self.lockname).await {
                Ok((value, version)) => {
                    //lock is inside
                    if value == "" {
                        //acquire the lock!
                        match self.client.put(&self.lockname, &self.token.to_string(), version).await {
                            Ok(()) => {
                                return;
                            },
                            Err(e) => {
                                //on version error, probably someone else acquired the lock between
                                // this get and put call
                                //so just retry

                                //on maybe error check the lock again to see if we held it
                                if e == KVError::Maybe {
                                    match self.client.get(&self.lockname).await {
                                        Ok((new_value, new_version)) => {
                                            if new_value == self.token.to_string() && new_version == (version + 1) {
                                                return;
                                            }
                                        }
                                        Err(_) => {
                                            //did not work, lets just retry
                                        }
                                    }
                                }
                            }
                        }
                    }
                    //otherwise, the lock is being held, so we just retry
                },
                Err(e) => {
                    if e == KVError::NoKey {
                        //add new empty lock and acquire!
                        match self.client.put(&self.lockname, &self.token.to_string(), 0).await {
                            Ok(()) => {
                                return;
                            },
                            Err(ee) => {
                                //on version error, probably someone else acquired the lock between
                                // this get and put call
                                //so just retry

                                //on maybe error check the lock again to see if there is a holder
                                if ee == KVError::Maybe {
                                    match self.client.get(&self.lockname).await {
                                        Ok((new_value, new_version)) => {
                                            if new_value == self.token.to_string() && new_version == 1 {
                                                return;
                                            }
                                        }
                                        Err(_) => {
                                            //did not work, lets just retry
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
            
        }
    }

    pub async fn release(&mut self) {
        // TODO: release the lock if this client owns it.
        loop {
            match self.client.get(&self.lockname).await {
                Ok((value, version)) => {
                    //lock is 
                    if value == self.token.to_string() {
                        //release the lock
                        match self.client.put(&self.lockname, "", version).await {
                            Ok(()) => {
                                return;
                            },
                            Err(e) => {
                                //on maybe error check the lock again to see if we properly released it
                                if e == KVError::Maybe {
                                    match self.client.get(&self.lockname).await {
                                        Ok((new_value, new_version)) => {
                                            if new_value == "" && new_version == (version + 1) {
                                                return;
                                            }
                                        }
                                        Err(_) => {
                                            //did not work, lets just retry
                                        }
                                    }
                                }
                                //for any other error just retry
                            }
                        }
                    } else if value == "" {
                        //already released from a succesful maybe
                        return;
                    }
                },
                Err(e) => {
                    //lock does not exist? so just keep going
                    if e == KVError::NoKey {
                        return;
                    }
                    //otherwise retry (in the maybe case)
                }
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    }
}
