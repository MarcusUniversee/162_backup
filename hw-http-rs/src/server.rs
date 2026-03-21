use std::env;
use std::net::{Ipv4Addr, SocketAddrV4};

use crate::args;

use crate::http::*;
use crate::stats::*;

use clap::Parser;
use tokio::net::{TcpListener, TcpStream};
use tokio::fs::File;
use tokio::fs;
use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;
use std::path::Path;
use std::ffi::OsStr;

use anyhow::Result;

pub fn main() -> Result<()> {
    // Configure logging
    // You can print logs (to stderr) using
    // `log::info!`, `log::warn!`, `log::error!`, etc.
    env_logger::Builder::new()
        .filter_level(log::LevelFilter::Info)
        .init();

    // Parse command line arguments
    let args = args::Args::parse();

    // Set the current working directory
    env::set_current_dir(&args.files)?;

    // Print some info for debugging
    log::info!("HTTP server initializing ---------");
    log::info!("Port:\t\t{}", args.port);
    log::info!("Num threads:\t{}", args.num_threads);
    log::info!("Directory:\t\t{}", &args.files);
    log::info!("----------------------------------");

    // Initialize a thread pool that starts running `listen`
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .worker_threads(args.num_threads)
        .build()?
        .block_on(listen(args.port))
}

async fn listen(port: u16) -> Result<()> {
    // Hint: you should call `handle_socket` in this function.
    let listener = TcpListener::bind(format!("127.0.0.1:{}", port)).await?;
    loop {
        let (socket, _) = listener.accept().await?;
        tokio::spawn(async move {
            handle_socket(socket).await
        });
    }
}

// Handles a single connection via `socket`.
async fn handle_socket(mut socket: TcpStream) -> Result<()> {
    let req = parse_request(&mut socket).await?;
    let path_str = format!(".{}", req.path);
    let relative_path = Path::new(&path_str);
    let relative_path_str = relative_path.as_os_str().to_str().unwrap();
    let file_type;
    match fs::metadata(relative_path_str).await {
        Ok(metadata) => {
            file_type = metadata.file_type();
        }
        Err(_) => {
            let status_code = 404;
            start_response(&mut socket, status_code).await?;
            return Ok(());
        }
    }

    if file_type.is_dir() {
        let mut dir = fs::read_dir(relative_path_str).await?;
        let index_path = format_index(relative_path_str);
        match File::open(&index_path).await {
            Ok(mut file) => {
                //index html exists
                let content_type = get_mime_type(&index_path);
                handle_file(&mut socket, &mut file, &content_type).await?;
            }
            Err(_) => {
                let status_code = 200;
                start_response(&mut socket, status_code).await?;
                send_header(&mut socket, "Content-Type", "text/html").await?;
                end_headers(&mut socket).await?;
                
                while let Some(entry) = dir.next_entry().await? {
                    let file_path = entry.path();
                    let file_path_str = file_path.as_os_str().to_str().unwrap();
                    let name = entry.file_name();
                    let name_str = name.as_os_str().to_str().unwrap();
                    let link = format_href(&file_path_str, &name_str);
                    socket.write_all((&link).as_bytes()).await?;
                }
                let parent = "parent";
                let parent_link = format_href(&relative_path_str, &parent);
                socket.write_all((&parent_link).as_bytes()).await?;
            }
        }

    } else {
        let content_type = get_mime_type(&relative_path_str);
        match File::open(relative_path_str).await {
            Ok(mut file) => {
                handle_file(&mut socket, &mut file, &content_type).await?;
            }
            Err(_) => {
                let status_code = 404;
                start_response(&mut socket, status_code).await?;
                return Ok(());
            }
        }
    }
    
    Ok(())
}

async fn handle_file(socket: &mut TcpStream, file: &mut File, content_type: &str) -> Result<()> {
    let status_code = 200;
    let metadata = file.metadata().await?;
    let length = metadata.len();
    start_response(socket, status_code).await?;
    send_header(socket, "Content-Type", content_type).await?;
    send_header(socket, "Content-Length", &length.to_string()).await?;
    end_headers(socket).await?;
    //create a array of 1024 bytes
    let mut buf = vec![0u8; 1024];
    loop {
        let bytes_read = file.read(&mut buf).await?;
        if bytes_read == 0 {
            break;
        }
        socket.write_all(&buf[..bytes_read]).await?;
    }
    Ok(())
}

// You are free (and encouraged) to add other funtions to this file.
// You can also create your own modules as you see fit.
