use std::collections::{HashMap, HashSet};
use std::fmt;
use std::str::FromStr;
use sscanf::sscanf;
use serde::ser::SerializeSeq;
use serde::{Deserialize, Deserializer, Serialize, Serializer};
use serde_with::{DeserializeFromStr, SerializeDisplay};

use crate::utils;
use crate::config::*;

// --- ATTACK CONFIG ---
//main structure for attack_config.toml
#[derive(Deserialize)]
pub struct AttackConfig {
  pub hammer_count: u64,
  pub segment_virt_addr: u64,
  pub victim_frames: Vec<VictimFrame>,
  aggressor_patterns: HashMap<String, AggressorPattern>
}

#[derive(Deserialize)]
pub struct VictimFrame {
  pub page_file_offset: Option<u64>,
  pub frame_addr: u64,
  pub victim_bits: Vec<VictimBit>
}

#[derive(Deserialize)]
pub struct VictimBit {
  pub offset: u64, //offset in page
  pub bitflip: BitFlip,
  pub aggr_pattern_key: String,
}

#[derive(DeserializeFromStr, PartialEq, Eq, Hash, Clone, Default)]
pub struct BitFlip {
  pub flip_index: u8, //offset in byte
  pub flip_direction: bool, //true = 0->1 ; false = 1->0
}

impl fmt::Display for BitFlip {
  fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
    write!(f, "{}{}", self.flip_index, if self.flip_direction {"+"} else {"-"})
  }
}

impl FromStr for BitFlip {
  type Err = std::string::FromUtf8Error; //some random error, not used

  fn from_str(s: &str) -> Result<Self, Self::Err> {
    assert!(s.len() == 2);
    Ok(BitFlip {
      flip_index: s.chars().nth(0).unwrap().to_digit(10).unwrap() as u8,
      flip_direction: s.chars().nth(1).unwrap() == '+'
    })
  }
}

#[derive(SerializeDisplay, DeserializeFromStr, PartialEq, Eq, Hash, Default, Clone)]
//#[serde(transparent)]
pub struct AggressorPattern {
  pub pattern: Vec<RowAndInitValue>
}

impl AggressorPattern {
  pub fn aggr_rows_to_virt(&self, frame2map: &Frame2Map) -> Vec<u64> {
    self.pattern.iter()
      .map(|x| frame2map[&x.frames[0]].data() as u64)
      .collect::<Vec<_>>()
  }
}

impl<'a> IntoIterator for &'a AggressorPattern {
  type Item =&'a RowAndInitValue;
  type IntoIter = std::slice::Iter<'a, RowAndInitValue>;

  //iter over RowAndInitValues
  fn into_iter(self) -> Self::IntoIter {
    self.pattern.iter()
  }
}

impl fmt::Display for AggressorPattern {
  fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
    let mut s = String::new();
    let mut sep = "";
    for a in &self.pattern {
      s += &format!("{}0x{:x}(0x{:x})", sep, a.frames[0], a.init);
      sep = ",";
    }
    write!(f, "{}", s)
  }
}

impl FromStr for AggressorPattern {
  type Err = std::string::FromUtf8Error; //some random error, not used

  fn from_str(s: &str) -> Result<Self, Self::Err> {
    Ok(AggressorPattern {pattern: s.split(",").map(|x| {
      let tmp = sscanf!(x, "0x{:x}(0x{:x})", u64, u8).unwrap();
      RowAndInitValue {frames: utils::get_frames_in_row(tmp.0), init: tmp.1}
    }).collect()})
  }
}

#[derive(Clone, Serialize, Deserialize, PartialEq, Eq, Hash)]
pub struct RowAndInitValue {
  #[serde(rename = "phys_aggr")] //for attack_config.toml
  #[serde(deserialize_with = "deserialize_row")]
  pub frames: Vec<u64>, //all physical frames in the row
  #[serde(rename = "aggr_init")] //for attack_config.toml
  pub init: u8
}

fn deserialize_row<'de, D>(d: D) -> Result<Vec<u64>, D::Error>
where D: Deserializer<'de> {
  let aggr_addr = u64::deserialize(d)?;
  Ok(utils::get_frames_in_row(aggr_addr))
}

impl AttackConfig {
  pub fn validate(self) -> AttackConfig {
    //1. in an attack scenario, there should be no overlap between victim and
    //aggressor rows, because the victims will contain actual code/data and can
    //therefore not be initialized to the aggressor init value
    let aggrs: HashSet<u64> = self.iter_aggr_frames().collect();
    let victims: HashSet<u64> = self.victim_frames.iter().map(|x| {
      utils::get_frames_in_row(x.frame_addr)
    }).flatten().collect();
    let t = aggrs.intersection(&victims)
      .fold(String::new(), |acc, x| acc + &format!("0x{:x} ", x));
    //overlap is checked with both pages in the row! it might look like an
    //aggressor does not overlap with a victim bcs the victim bit is in the
    //second page, which is still an overlap!
    assert!(t == "",
      "There is overlap between victim and aggressor rows: {}", t);

    //2. check that mappings at the same file page offset are the same frame
    //it does not make sence to include the same mapping (file offset and
    //victim frame) multiple times with a different collection of victim bits
    //(instead include all victim bits together in one mapping structure)
    //=> only check on the uniqueness of the file offsets
    let mut tmp = HashSet::new();
    self.victim_frames.iter().for_each(|x| {
      assert!(tmp.insert(x.page_file_offset.unwrap() & PAGE_ALIGN_MASK),
        "The same file page is mapped to different physical page frames");
    });

    //3. check there are no duplicate aggressor patterns
    let mut tmp = HashSet::new();
    for (_,v) in &self.aggressor_patterns {
      assert!(tmp.insert(v), "Duplicate aggressor pattern detected: {}", v)
    }

    //4. check that the same aggressor row in different patterns have the same
    //init value (because in the rowhammer tools (usually) all aggressor rows
    //are initialized together at the start (otherwise the last init value of
    //some aggressor row overwrites the earlier init value of the same row)
    let mut tmp = HashMap::new();
    let mut vec = Vec::new();
    for (_,v) in &self.aggressor_patterns {
      for aggr in &v.pattern {
        if let Some(i) = tmp.get(&aggr.frames[0]) {
          if *i != aggr.init {vec.push(aggr.frames[0])};
        } else {
          tmp.insert(aggr.frames[0], aggr.init);
        }
      }
    }

    assert!(vec.len() == 0, "Some aggressor rows are used with different \
      init values: {}", vec.iter().map(|x| format!("0x{:x}", x))
      .fold(String::new(), |acc, x| acc + " " + &x));

    self
  }

  //can contain duplicates
  pub fn iter_aggr_patterns_per_victim(
    &self
  ) -> impl Iterator<Item=&AggressorPattern> {
    self.victim_frames.iter()
      .map(|a| &a.victim_bits).flatten()
      .map(move |b| self.aggressor_patterns.get(&b.aggr_pattern_key).unwrap())
  }

  //iter all rows used as aggressor
  //can contain duplicates (potentially with different init value!!) if same
  //row is used in multiple patterns!
  pub fn iter_aggr_rows(&self) -> impl Iterator<Item=&RowAndInitValue> + '_ {
    self.aggressor_patterns.values().flatten()
  }

  //iterator over all frames that are used as an aggressor (all frames in all
  //aggr rows)
  pub fn iter_aggr_frames(&self) -> impl Iterator<Item=u64> + '_ {
    self.iter_aggr_rows().map(|x| &x.frames).flatten().cloned()
  }
}


// --- TEMPLATER ---
//main structure for templater_config.toml
#[derive(Serialize, Deserialize)]
pub struct TemplaterConfig {
  pub row_start: u64,
  pub row_end: u64,
  pub bank_idxs: Vec<u64>,
  pub hammer_count: u64,
  pub garbage_count_start: u32,
  pub garbage_count_end: u32,
  pub drop_frac: f64,
  pub init_values: Vec<VicAggrInit>,
  pub repetition: usize,
  pub pattern: String
}

#[derive(Serialize, Deserialize)]
pub struct VicAggrInit {
  pub victim_init: u8,
  pub aggr_init: u8
}

#[derive(Serialize, Deserialize)]
pub struct MemoryTemplate {
  pub templater_config: TemplaterConfig,
  pub dram_config: DRAMConfig,
  pub host_config: HostConfig,
  pub timestamp: String,
  pub comment: String,
  #[serde(deserialize_with = "deserialize_victims")]
  #[serde(serialize_with = "serialize_victims")]
  pub victims: Vec<(u64, BitFlip, usize)>, //(phys_addr, flip, aggr_pattern_idx)
  pub aggr_patterns: Vec<AggressorPattern>,
  pub distribution: HashMap<u32, Vec<Vec<usize>>>
}

fn deserialize_victims<'de, D>(d: D)
  -> Result<Vec<(u64, BitFlip, usize)>, D::Error>
where D: Deserializer<'de> {
  let tmp: Vec<String> = Vec::deserialize(d)?;
  let mut res = Vec::new();
  for t in tmp {
    let r: Vec<&str> = t.split(" ").collect();
    let flip = BitFlip::from_str(r[1]).unwrap();
    res.push((u64::from_str_radix(&r[0][2..], 16).unwrap(),
      flip, usize::from_str(r[2]).unwrap()));
  }
  Ok(res)
}

fn serialize_victims<S>(v: &Vec<(u64, BitFlip, usize)>, s: S)
  -> Result<S::Ok , S::Error>
where S: Serializer {
  let mut ser = s.serialize_seq(Some(v.len()))?;
  for (phys_addr, flip, aggr_idx) in v {
    let tmp = format!("0x{:x} {} {}", phys_addr, flip, aggr_idx);
    ser.serialize_element(&tmp)?;
  };

  ser.end()
}


// --- DRAM CONFIG ---
//main structure for dram_config.toml with DRAM to/from physical address
//translation functions
#[derive(Serialize, Deserialize)]
pub struct DRAMConfig {
  pub dram_id: String,
  pub row_fn: u64,
  pub column_fn: u64,
  pub bank_fns: Vec<u64>
}

#[derive(Serialize, Deserialize)]
pub struct DRAMInfo {
  pub dram_ids: HashMap<String, String>,
  pub mapping_functions: HashMap<String, MappingFunctions>,
}

#[derive(Serialize, Deserialize)]
pub struct MappingFunctions {
  pub row_fn: u64,
  pub column_fn: u64,
  pub bank_fns: Vec<u64>
}

#[derive(Serialize, Deserialize)]
pub struct HostConfig {
  pub hostname: String,
  pub motherboard_id: String,
  pub cpu_model: String,
  pub smt_disabled: bool,
  pub turbo_disabled: bool,
  //[(cpuid, info)] info = is_isolated,scaling_governor,min&max_freq
  pub cpu_ids: Vec<(u32, String)>,
  pub git_hash: String, //not really "host" config
  pub ram_swap_id: u32,
  pub os_release: String,
  pub kernel: String
}

