use std::env;
use std::time::{Duration, SystemTime};
use log::*;

use rowhammer::*;
use rowhammer::config::*;
use rowhammer::utils::{*, serialize::*};


/*
 * This library implements the loader functionality that loads the target pages
 * into the right victim frame
 * -- Target Binary --
 * The target binary should contain exactly one PT_NULL segment with all code
 * containing targets. The main function should reside in a PT_LOAD segment
 * because it is used as the entry point
 * At startup, the target binary's `main` function should first call the
 * `do_the_thing` function in this library to activate the loader's
 * functionality before executing anything else
 * -- Physical Memory Loader
 * This loader performs the following steps:
 *   1. It allocates pages until all required physical frames are found
 *      (both victim and aggressor frames)
 *   2. It maps all PT_NULL segment pages (page by page) to some unmapped pages
 *   3. To put each target page in its corresponding victim frame, the
 *      loader memcpys each target page to the page (allocated in step 1) that
 *      resides in the right victim frame
 *   4. Lastly, all mapped pieces from the PT_NULL segment are mremapped to
 *      forge a contiguous virtual address space starting at a specified
 *      virtual address (chosen by the linker and provided in the
 *      attack_config.toml file). After this step, the pages are still in the
 *      same physical frames so the targets reside in the victim spots and the
 *      hammering can start
 *   5. Rowhammer. Hammering happens in the same process but it could be moved
 *      to a separate process. When control flows back to `main`, it can
 *      redirect execution into the forged part (the `old_main` function)
 *
 * When the environment variable `RH_TEST` is defined, the loader component will
 * just return without any action
 */

//entry for the protected program to invoke the loader
#[no_mangle]
pub extern "C" fn do_the_thing() {
  //skip the whole loader by setting the `RH_TEST` environment variable
  //this is useful when, e.g., all code pages are in a PT_LOAD segment (and
  //thus loaded automatically by the system loader) and no bit flips are
  //required
  if env::var("RH_TEST").is_ok() {
    return;
  }

  // 0. Preparations
  rowhammer::configure().init();
  let attack_config =
    files::parse_toml::<AttackConfig>(ATTACK_CONFIG_PATH).validate();
  let dram_config = dram::create_config();

  // 1. Allocate until a page landed in all victim frames
  let mut start_time = SystemTime::now();
  let mut frame2map = allocation::allocate_attack(
    &dram_config, &attack_config);
  let mut duration = SystemTime::now().duration_since(start_time).unwrap();
  info!("Allocation took {}ms", duration.as_millis());

  // 2. Mapping the PT_NULL segment and forging part of the address space
  start_time = SystemTime::now();
  let program_path = std::env::args().next().unwrap();
  allocation::map_binary(&program_path, &attack_config, &mut frame2map);
  duration = SystemTime::now().duration_since(start_time).unwrap();
  info!("Mapping took {}ms", duration.as_millis());

  let content_before_rh =
    mem_init::read_victim_content(attack_config.victim_frames.iter(), &frame2map);

  // 3. RowHammer all aggressors at once
  start_time = SystemTime::now();
  //bcs the attack config is validated, victims and aggressors do not overlap,
  //thus all aggressors can be initialized at once
  mem_init::initialize_rows(attack_config.iter_aggr_rows(), &frame2map);
  duration = SystemTime::now().duration_since(start_time).unwrap();
  info!("Aggr init took {}ms", duration.as_millis());

  let sleep = Duration::new(2, 0);

  start_time = SystemTime::now();
  attack_config.iter_aggr_patterns_per_victim().for_each(|v| {
    let virt_aggrs = v.aggr_rows_to_virt(&frame2map);
    std::thread::sleep(sleep);
    //hammer::hammer_rust(&virt_aggrs, attack_config.hammer_count);
    //hammer::hammer_asm(&virt_aggrs, 35, attack_config.hammer_count);
    hammer::hammer_jit(&virt_aggrs, 35, attack_config.hammer_count);
  });
  duration = SystemTime::now().duration_since(start_time).unwrap();
  info!("Hammering took {}ms", duration.as_millis());

  // 4. Check if target content changed
  flip_checks::check_victim_flips(
    attack_config.victim_frames.iter(), &frame2map, &content_before_rh);

  info!("Returning to target binary");

  //frame2map owns the MemoryMap objects for the pages containing the program's
  //binary code. These pages have to stay mapped after the loader finishes!!
  for (_, page) in frame2map.into_iter() {
    page.lose_page();
  }

  //drop root privileges
  //TODO test if it works
  //this program runs with root privileges so setuid sets RUID, EUID and SUID
  //CAREFUL!! I use the SUDO_UID environment variable but, depending on the
  //settings of the security policy (e.g. sudoers), the user can change this
  //value!
  //for non-system services, maybe the info from systemd-logind is better???
  //(https://stackoverflow.com/a/63966477)
  //`logname` and `who ...` are hacks that look at the terminal connected to
  //stdin (does not work when stdin is redirected)
  //this won't work when the program is launched by the root user, but then
  //dropping root privileges doesn't make sense)
  //Matheo has a better idea for this: make the binary a SUID program owned by
  //root (so a user can launch it without sudo) and drop privileges with 
  //setresuid(getuid(), getuid(), getuid())
  let sudo_uid = env::var("SUDO_UID").unwrap().parse::<u32>().unwrap();
  unsafe {assert!(libc::setuid(sudo_uid) == 0);}
}

