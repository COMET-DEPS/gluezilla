use serde::de::DeserializeOwned;
use std::fs;
use log::*;


//#[inline(always)]
pub fn parse_toml<T: DeserializeOwned>(path: &str) -> T {
  info!("Parsing toml file: {}", path);
  let config_string = fs::read_to_string(path).unwrap();
  toml::from_str::<T>(&config_string).unwrap()
}

pub fn parse_json<T: DeserializeOwned>(path: &str) -> T {
  info!("Parsing json file: {}", path);
  let config_string = fs::read_to_string(path).unwrap();
  serde_json::from_str::<T>(&config_string).unwrap()
}

