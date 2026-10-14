//! Native, on-device extraction of Wii disc contents from ISO, WBFS and RVZ.
//! File descriptor supplied by Storage Access Framework; no intermediate ISO is created.
//! Uses the MIT/Apache-2.0 licensed nod library from encounter/nod.
use std::{
    fs::{self, File},
    io::{self, Write},
    os::fd::{BorrowedFd, FromRawFd},
    path::{Component, Path, PathBuf},
};

use jni::{
    JNIEnv,
    objects::{JClass, JObject, JString, JValue},
    sys::{jint, jstring},
};
use nod::{
    common::PartitionKind,
    read::{DiscOptions, DiscReader, PartitionOptions},
};

fn validate_component_path(path: &str) -> Result<&Path, String> {
    let path = Path::new(path);
    if path.as_os_str().is_empty()
        || !path.components().all(|c| matches!(c, Component::Normal(_)))
    {
        return Err(format!("Unsafe path in disc: {}", path.display()));
    }
    Ok(path)
}

fn notify(env: &mut JNIEnv<'_>, callback: &JObject<'_>, current: i32, total: i32, name: &str) {
    if let Ok(message) = env.new_string(name) {
        let string_obj = JObject::from(message);
        let _ = env.call_method(
            callback, "update", "(IILjava/lang/String;)V",
            &[
                JValue::Int(current),
                JValue::Int(total),
                JValue::Object(&string_obj),
            ],
        );
        let _ = env.exception_clear();
    }
}

fn write_sys(root: &Path, name: &str, data: &[u8]) -> Result<(), String> {
    fs::write(root.join("sys").join(name), data).map_err(|e| format!("Write {}: {e}", name))
}

fn extract_disc(
    env: &mut JNIEnv<'_>,
    fd: jint,
    root: &Path,
    callback: JObject<'_>,
) -> Result<usize, String> {
    if fd < 0 {
        return Err("Storage provider did not return a readable file descriptor".into());
    }
    // Dup the Java-owned fd. Closing the Rust File must not close Android's document fd.
    let owned = unsafe { BorrowedFd::borrow_raw(fd) }
        .try_clone_to_owned()
        .map_err(|e| format!("Cannot duplicate file descriptor: {e}"))?;
    let input = File::from(owned);
    let disc = DiscReader::new_from_non_cloneable_read(input, &DiscOptions::default())
        .map_err(|e| format!("Failed to read disc: {e}"))?;
    let header = disc.header();
    if !header.is_wii() {
        return Err("This is not a Wii disc".into());
    }
    if header.game_id_str() != "SMNE01" || header.disc_version != 1 {
        return Err(format!(
            "This port requires New Super Mario Bros. Wii USA (SMNE01) revision 1; received {} revision {}",
            header.game_id_str(), header.disc_version
        ));
    }
    notify(env, &callback, 0, 0, "Opening Wii game partition…");
    let mut partition = disc
        .open_partition_kind(PartitionKind::Data, &PartitionOptions::default())
        .map_err(|e| format!("Cannot open Wii game partition: {e}"))?;
    let meta = partition.meta().map_err(|e| format!("Invalid game partition: {e}"))?;
    if meta.raw_boot.get(0..8).is_none_or(|bytes| &bytes[0..6] != b"SMNE01" || bytes[7] != 1) {
        return Err("Game data partition revision does not match SMNE01 revision 1".into());
    }

    fs::create_dir_all(root.join("sys")).map_err(|e| format!("Create sys directory: {e}"))?;
    fs::create_dir_all(root.join("files")).map_err(|e| format!("Create files directory: {e}"))?;

    // Matches the data and metadata layout produced by nodtool extract / Dolphin.
    write_sys(root, "boot.bin", &meta.raw_boot[..])?;
    write_sys(root, "bi2.bin", &meta.raw_bi2[..])?;
    write_sys(root, "apploader.img", &meta.raw_apploader[..])?;
    write_sys(root, "main.dol", &meta.raw_dol[..])?;
    write_sys(root, "fst.bin", &meta.raw_fst[..])?;

    let disc_root = root.join("disc");
    fs::create_dir_all(&disc_root).map_err(|e| format!("Create disc directory: {e}"))?;
    fs::write(disc_root.join("header.bin"), &header.game_id[..])
        .map_err(|e| format!("Write header: {e}"))?;
    if let Some(region) = disc.region() {
        fs::write(disc_root.join("region.bin"), region)
            .map_err(|e| format!("Write region: {e}"))?;
    }
    for (name, bytes) in [
        ("ticket.bin", meta.raw_ticket.as_deref()),
        ("tmd.bin", meta.raw_tmd.as_deref()),
        ("cert.bin", meta.raw_cert_chain.as_deref()),
        ("h3.bin", meta.raw_h3_table.as_deref().map(|x| &x[..])),
    ] {
        if let Some(bytes) = bytes {
            fs::write(root.join(name), bytes).map_err(|e| format!("Write {name}: {e}"))?;
        }
    }

    let fst = meta.fst().map_err(|e| format!("Invalid Wii file table: {e}"))?;
    let total = fst.num_files();
    if total > 150_000 {
        return Err("Implausibly large file table".into());
    }

    let mut count = 5usize;
    let mut done = 0;
    for (_, node, path) in fst.iter() {
        let path = validate_component_path(&path)?;
        let output = root.join("files").join(path);
        if node.is_dir() {
            fs::create_dir_all(&output)
                .map_err(|e| format!("Create {}: {e}", output.display()))?;
        } else {
            if let Some(parent) = output.parent() {
                fs::create_dir_all(parent)
                    .map_err(|e| format!("Create {}: {e}", parent.display()))?;
            }
            let mut out = File::create(&output)
                .map_err(|e| format!("Create {}: {e}", output.display()))?;
            let mut file = partition.open_file(node)
                .map_err(|e| format!("Read {}: {e}", path.display()))?;
            io::copy(&mut file, &mut out)
                .map_err(|e| format!("Extract {}: {e}", path.display()))?;
            out.flush().map_err(|e| format!("Flush {}: {e}", path.display()))?;
            count += 1;
            done += 1;
            if done == 1 || done % 5 == 0 || done == total {
                notify(env, &callback, done as i32, total as i32,
                    &format!("Extracting {done}/{total}: {}", path.display()));
            }
        }
    }
    notify(env, &callback, total as i32, total as i32, "Game data extracted");
    Ok(count)
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_com_wither_nsmbw_NativeDiscExtractor_extract(
    mut env: JNIEnv,
    _class: JClass,
    fd: jint,
    destination: JString,
    callback: JObject,
) -> jstring {
    let path = env.get_string(&destination)
        .map(|s| PathBuf::from(s.to_string_lossy().as_ref()));
    let result = match path {
        Ok(path) => std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            extract_disc(&mut env, fd, &path, callback)
        }))
        .unwrap_or_else(|_| Err("Native disc importer encountered an internal error".into())),
        Err(e) => Err(format!("Invalid destination path: {e}")),
    };
    let message = match result {
        Ok(n) => format!("OK:{n}"),
        Err(e) => format!("ERROR:{e}"),
    };
    match env.new_string(message) {
        Ok(s) => s.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}
