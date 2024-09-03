use std::ffi::OsStr;
use env_logger::Env;
use nix::unistd::getuid;
use nix::sys::mman;


//This library contains all reusable functions to build rowhammer tools with
//Some may seem a bit arbitrary, that's because they are
pub mod config;
pub mod utils;
pub mod allocation;
pub mod flip_checks;
pub mod mem_init;
pub mod hammer;
use config::*;

//Some basic checks every tool should perform
//Returns an env_logger::Builder instance for further tool-specific logger
//configurations, call `init()` on it to finish the creation of the instance
pub fn configure() -> env_logger::Builder {
  let hostname = hostname::get().expect("Could not get hostname");
  for name in FORBIDDEN_HOSTNAMES {
    assert!(hostname != OsStr::new(name),
      "No Ruben, you don't want to run this on {}...", name);
  }

  assert!(getuid().is_root(), "Program should run with root privileges");

  //lock all pages of this process in physical memory (= prevent swapping)
  mman::mlockall(mman::MlockAllFlags::all()).expect("mlockall failed");

  let mut logger_builder =
    env_logger::Builder::from_env(Env::default().default_filter_or("info"));
  logger_builder.format_timestamp(None);
  //logger_builder.format(|buf, record| writeln!(buf, "{}", record.args()));
  logger_builder
}




