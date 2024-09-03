pub mod proc;
pub mod serialize;
pub mod dram;
pub mod files;
pub mod host;
pub mod garbage;
pub mod devmem;

use std::process::Command;
use regex::{Regex, Captures};
use log::*;

use crate::config::*;

//Impotant note! Running commands without an absolute path is a security issue
//(especially given this code runs in a privileged context) because it relies 
//on the PATH env variable to locate the programs
//A malicous actor, however, can manipulate this variable to launch arbitrary 
//programs
//We do not consider this situation in this proof-of-concept implementation

//Clear page cache
pub fn clear_page_cache() {
  info!("Clearing the page cache");
  let mut cmd = Command::new("su");
  cmd.args(&["-c", "sync && echo 1 > /proc/sys/vm/drop_caches"]);
  let status = cmd.status()
                  .expect("Clearing the page cache failed with IO error");
  assert!(status.success(), "Clearing the page cache failed");
}

pub fn get_frames_in_row(row_phys_addr: u64) -> Vec<u64> {
  let row_start = row_phys_addr & ROW_ALIGN_MASK;
  let row_end = row_start + ROW_SIZE;
  (row_start..row_end).step_by(PAGE_SIZE).collect()
}

//arg::command = [command, arg1, arg2, ...]
//returns stdout
pub fn run(command: &[&str]) -> String {
  let cmd = Command::new(command[0]).args(&command[1..]).output()
    .expect(&format!("command `{}` failed", command.join(" ")));
  let stdout = String::from_utf8(cmd.stdout)
    .expect(&format!("stdout of command `{}` is not UTF8", command.join(" ")));
  let stderr = String::from_utf8(cmd.stderr)
    .expect(&format!("stderr of command `{}` is not UTF8", command.join(" ")));
  if !stderr.is_empty() {
    warn!("Command {} stderr: {}", command.join(" "), stderr);
  }
  stdout
}

//can also use sscanf!
//returns [match1: Cap[group1, group2, ...], match2: Cap[...]]
pub fn regex<'t>(text: &'t str, regex: &str) -> Vec<Captures<'t>> {
  let reg = Regex::new(regex).expect(&format!("Regex `{}` invalid", regex));
  reg.captures_iter(text).collect()
}

