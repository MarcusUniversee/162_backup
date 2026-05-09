use crate::persister::Persister;
use crate::rpc::{RpcClient, Server};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Duration;
use rand::prelude::*;
use std::cmp;

#[derive(Serialize, Deserialize)]
#[derive(Clone)] //so that we can copy it
pub struct LogEntry {
    //each entry contains command for state machine, and term when entry was received by leader (first index is 1)
    pub command: String,
    pub term: u64,
}
// TODO: define RPC request/reply types.

#[derive(Serialize, Deserialize)]
pub struct RequestVoteArgs {
    pub term: u64,
    pub candidate_id: usize,
    pub last_log_index: u64,
    pub last_log_term: u64,
}

#[derive(Serialize, Deserialize)]
pub struct RequestVoteReply {
    pub term: u64,
    pub vote_granted: bool,
}

#[derive(Serialize, Deserialize)]
pub struct AppendEntriesArgs {
    pub term: u64,
    pub leader_id: usize,
    pub prev_log_index: u64,
    pub prev_log_term: u64,
    pub entries: Vec<LogEntry>,
    pub leader_commit: u64,
}

#[derive(Serialize, Deserialize)]
pub struct AppendEntriesReply {
    pub term: u64,
    pub success: bool,
}

#[derive(Serialize, Deserialize)]
struct PersistState {
    //persistent
    pub current_term: u64,
    pub voted_for: Option<usize>, //option since null if None
    pub log: Vec<LogEntry>,
}

// TODO: define persistent state.
pub struct RaftState {
    //persistent
    pub current_term: u64,
    pub voted_for: Option<usize>, //option since null if None
    pub log: Vec<LogEntry>,
    //volatile
    pub commit_index: u64,
    pub last_applied: u64,
    pub role: u8, //0 is follower, 1 is candidate, 2 is leader
    pub time: u64,
    pub timeout: u64,
    //for leaders
    pub next_index: Vec<u64>,
    pub match_index: Vec<u64>,
}

pub struct Raft {
    me: usize,
    peers: Vec<Arc<RpcClient>>,
    persister: Arc<Persister>,
    // TODO: add persistent and volatile Raft state.
    state: Mutex<RaftState>,
}

impl RaftState {
    pub fn new() -> Self {
        Self {
            current_term: 0,
            voted_for: None,
            log: vec![],
            commit_index: 0,
            last_applied: 0,
            role: 0,
            time: 0,
            timeout: 0,
            next_index: vec![],
            match_index: vec![],
        }
    }
}

impl Raft {
    /// Create a new Raft peer.
    pub fn new(me: usize, peers: Vec<Arc<RpcClient>>, persister: Arc<Persister>) -> Arc<Self> {
        // TODO: start background tasks after state initialization.
        let raft = Arc::new(Self {
            me,
            peers,
            persister,
            state: Mutex::new(RaftState::new()),
        });
        raft.unpersist();
        let self_ = raft.clone();
        tokio::spawn(async move {
            self_.state.lock().unwrap().timeout = rand::rng().random_range(200..=400);
            loop {
                let role = self_.state.lock().unwrap().role;
                if role == 0 {
                    let (time, timeout) = {
                        let state = self_.state.lock().unwrap();
                        (state.time, state.timeout)
                    };
                    if time >= timeout {
                        self_.state.lock().unwrap().time = 0;
                        self_.clone().start_election().await;
                    }
                } else if role == 1 {
                    //candidate
                    let (time, timeout) = {
                        let state = self_.state.lock().unwrap();
                        (state.time, state.timeout)
                    };
                    if time >= timeout {
                        self_.state.lock().unwrap().time = 0;
                        self_.clone().start_election().await;
                    }
                } else if role == 2{
                    //leader
                    let (time, timeout) = {
                        let state = self_.state.lock().unwrap();
                        (state.time, state.timeout)
                    };
                    if time >= 50 {
                        self_.state.lock().unwrap().time = 0;
                        self_.clone().send_heartbeats().await;
                    }
                }
                tokio::time::sleep(Duration::from_millis(10)).await;
                self_.state.lock().unwrap().time += 10;
            }
        });
        raft
    }

    fn persist(&self, state: &RaftState) {
        let persist_state = PersistState {
                current_term: state.current_term,
                voted_for: state.voted_for,
                log: state.log.clone(),
            };
        let data = serde_json::to_string(&persist_state).unwrap();
        self.persister.save(&data);
    }

    fn unpersist(&self) {
        let data = self.persister.read();
        if data == "" { //no data saved
            return;
        }
        let persist_state: PersistState = serde_json::from_str(&data).unwrap();
        let mut state = self.state.lock().unwrap();
        state.current_term = persist_state.current_term;
        state.voted_for = persist_state.voted_for;
        state.log = persist_state.log.clone();
    }

    //needed to change self to Arc<Self> so this function can terminate the spawns are still alive
    async fn start_election(self: Arc<Self>) {
        //begin an election
        //put the args in here so that we drop the lock after
        let (cur_term, last_log_index, last_log_term) = {
            let mut state = self.state.lock().unwrap();
            state.current_term += 1;
            state.voted_for = Some(self.me);
            state.role = 1;
            state.time = 0;
            state.timeout = rand::rng().random_range(200..=400);
            let last_term = if state.log.len() == 0 {0} else {state.log[state.log.len() - 1].term};
            (state.current_term, state.log.len() as u64, last_term)
        };
        //from example on struct Arc website
        //define shared information between peers
        let votes = Arc::new(AtomicUsize::new(1));
        let majority = self.peers.len() / 2 + 1;
        //issue request vote rpcs in parallel to other servers in cluster
        for (i, peer) in self.peers.iter().enumerate() {
            if i == self.me {
                continue;
            }
            //clone so we can move into the async
            let peer = peer.clone();
            let votes = votes.clone();
            let self_ = self.clone();
            let args = RequestVoteArgs {
                term: cur_term,
                candidate_id: self_.me,
                last_log_index: last_log_index,
                last_log_term: last_log_term,
            };
            
            tokio::spawn(async move {
                match peer.call::<RequestVoteArgs, RequestVoteReply>("request_vote", &args).await {
                    Some(reply) => {
                        if reply.term > cur_term {
                            let mut state = self_.state.lock().unwrap();
                            state.role = 0;
                            state.current_term = reply.term;
                            state.voted_for = None;
                            self_.persist(&state);
                        } else if reply.vote_granted {
                            let v = votes.fetch_add(1, Ordering::Relaxed) + 1; //add one since fetch add returns old value
                            if v >= majority {
                                //do this so we drop the lock outside block scope
                                let leader = {
                                    let mut state = self_.state.lock().unwrap();
                                    //make sure terms match (so it wasn't an old election)
                                    if state.role == 1 && state.current_term == cur_term {
                                        state.role = 2;
                                        state.next_index = vec![state.log.len() as u64 + 1; self_.peers.len()];
                                        state.match_index = vec![0; self_.peers.len()];
                                        state.match_index[self_.me] = state.log.len() as u64;
                                        true
                                    } else {
                                        false
                                    }
                                };
                                if leader {
                                    self_.send_heartbeats().await;
                                }
                            }
                        }
                    }
                    None => {}
                };
            });
        }
    }

    async fn send_heartbeats(self: Arc<Self>) {
        let cur_term = {
            let state = self.state.lock().unwrap();
            state.current_term
        };
        for (i, peer) in self.peers.iter().enumerate() {
            if i == self.me {
                continue;
            }
            
            //clone so we can move into the async
            let peer = peer.clone();
            let self_ = self.clone();
        
            tokio::spawn(async move {
                loop {
                    let (new_term, role) = {
                        let state = self_.state.lock().unwrap();
                        (state.current_term, state.role)
                    };
                    if role != 2 || new_term != cur_term {
                        break;
                    } 
                    let args = {
                        let state = self_.state.lock().unwrap();
                        let next_index = state.next_index[i];
                        let prev_log_index = next_index - 1;
                        //either empty, or vector of new entries
                        let entries: Vec<LogEntry> = if state.log.len() < next_index as usize {vec![]} else {state.log[prev_log_index as usize..].to_vec()};
                        AppendEntriesArgs {
                            term: cur_term,
                            leader_id: self_.me,
                            prev_log_index: prev_log_index,
                            prev_log_term: if prev_log_index <= 0 {0} else {state.log[prev_log_index as usize - 1].term},
                            entries: entries,
                            leader_commit: state.commit_index,
                        }
                    };
                    match peer.call::<AppendEntriesArgs, AppendEntriesReply>("append_entries", &args).await {
                        Some(reply) => {
                            if reply.term > cur_term {
                                let mut state = self_.state.lock().unwrap();
                                state.role = 0;
                                state.current_term = reply.term;
                                state.voted_for = None;
                                self_.persist(&state);
                                break;
                            }
                            if reply.success {
                                let mut state = self_.state.lock().unwrap();
                                let match_idx = args.prev_log_index + args.entries.len() as u64;
                                if match_idx > state.match_index[i] { //always monotonically increasing
                                    state.match_index[i] = match_idx;
                                    state.next_index[i] = state.match_index[i] + 1;
                                }
                                //from rust docs: rev reverses order (so goes from highest to lowest)
                                for n in ((state.commit_index as usize + 1)..=state.log.len()).rev() {
                                    if state.log[n - 1].term != state.current_term {
                                        continue;
                                    }
                                    //set commit index to n if majority
                                    let majority = state.match_index.len() / 2 + 1;
                                    let mut count: usize = 0;
                                    for idx in &state.match_index {
                                        if *idx >= n as u64 {
                                            count += 1;
                                        }
                                    }
                                    if count >= majority {
                                        state.commit_index = n as u64;
                                        break;
                                    }
                                }
                                break;
                            } else {
                                let mut state = self_.state.lock().unwrap();
                                if state.next_index[i] == 1 {
                                    break;
                                }
                                state.next_index[i] -= 1;
                            }
                        }
                        None => {break;}
                    }
                }

            });
        }
    }

    /// Return (current_term, is_leader).
    pub async fn get_state(&self) -> (u64, bool) {
        let state = self.state.lock().unwrap();
        let leader = state.role == 2;
        return (state.current_term, leader);
    }

    /// Submit a command to the log.
    pub async fn submit(&self, command: &str) -> Option<(usize, u64)> {
        let mut state = self.state.lock().unwrap();
        if state.role == 2 {
            let term = state.current_term;
            let entry = LogEntry {
                command: command.to_string(),
                term: term,
            };
            state.log.push(entry);
            self.persist(&state);
            let idx = state.log.len(); 
            state.match_index[self.me] = state.log.len() as u64;
            return Some((idx, term))
        }
        return None;
    }

    /// Return the committed command at `index`.
    pub async fn get_committed(&self, index: usize) -> Option<String> {
        let state = self.state.lock().unwrap();
        if index == 0 || index as u64 > state.commit_index {return None;}
        match state.log.get(index - 1) {
            Some(entry) => {Some(entry.clone().command)}
            None => {None}
        }
    }

    // TODO: define Raft RPC handlers.
    pub fn append_entries(&self, args: &AppendEntriesArgs) -> AppendEntriesReply {
        let mut state = self.state.lock().unwrap();
        if args.term > state.current_term {
            state.current_term = args.term;
            state.voted_for = None;
            state.role = 0;
            self.persist(&state);
        }
        if args.term < state.current_term {
            return AppendEntriesReply {
                term: state.current_term,
                success: false,
            }
        }
        state.time = 0;
        state.timeout = rand::rng().random_range(200..=400);
        if state.role == 1 {
            state.role = 0; //there is a leader now, so demote if we are candidate
        }
        if state.log.len() < args.prev_log_index as usize {
            return AppendEntriesReply {
                term: state.current_term,
                success: false,
            }
        }
        if args.prev_log_index > 0 {
            let ent = state.log.get(args.prev_log_index as usize - 1);
            if ent.is_none() {
                return AppendEntriesReply {
                    term: state.current_term,
                    success: false,
                }
            }

            if ent.unwrap().term != args.prev_log_term {
                return AppendEntriesReply {
                    term: state.current_term,
                    success: false,
                }
            }
        }
        let mut idx: usize = args.prev_log_index as usize + 1;
        for entry in &args.entries {
            match state.log.get((idx - 1) as usize) {
                Some(existing_entry) => {
                    //subtract 1 since the first index is 1?
                    if existing_entry.term != entry.term {
                        state.log.truncate(idx - 1);
                        state.log.push(entry.clone());
                        self.persist(&state);
                    }
                }
                None => {
                    state.log.push(entry.clone());
                    self.persist(&state);
                }
            }
            idx += 1;
        }
        if args.leader_commit > state.commit_index {
            state.commit_index = cmp::min(args.leader_commit, idx as u64 - 1);
        }
        return AppendEntriesReply {
            term: state.current_term,
            success: true,
        }
    }
    pub fn request_vote(&self, args: &RequestVoteArgs) -> RequestVoteReply {
        let mut state = self.state.lock().unwrap();
        if args.term > state.current_term {
            state.current_term = args.term;
            state.voted_for = None;
            state.role = 0;
            self.persist(&state);
        } 
        if args.term < state.current_term {
            return RequestVoteReply {
                term: state.current_term,
                vote_granted: false,
            }
        }
        //boolean that checks if the candidate is more up to date on logs
        let mut candidate_up_to_date = false;
        let my_latest_term = if state.log.len() != 0 {state.log[state.log.len() - 1].term} else {0};
        if args.last_log_term > my_latest_term {
            candidate_up_to_date = true;
        } else if args.last_log_term == my_latest_term {
            if args.last_log_index >= state.log.len() as u64 {
                candidate_up_to_date = true;
            }
        }
        if (state.voted_for.is_none() || state.voted_for == Some(args.candidate_id))
        && candidate_up_to_date  {
            state.time = 0;
            state.timeout = rand::rng().random_range(200..=400);
            state.voted_for = Some(args.candidate_id);
            return RequestVoteReply {
                term: state.current_term,
                vote_granted: true,
            }
        }
        return RequestVoteReply {
            term: state.current_term,
            vote_granted: false,
        }
    }
}

#[async_trait::async_trait]
impl Server for Raft {
    async fn dispatch(&self, name: &str, args: String) -> String {
        // TODO: dispatch Raft RPCs.
        match name {
            "append_entries" => {
                serde_json::to_string(&self.append_entries(&serde_json::from_str(&args).unwrap())).unwrap()
            }
            "request_vote" => {
                serde_json::to_string(&self.request_vote(&serde_json::from_str(&args).unwrap())).unwrap()
            }
            _ => panic!("unknown request name"),
        }
    }
}
