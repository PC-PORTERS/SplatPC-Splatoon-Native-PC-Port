use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use boringtun::noise::{Tunn, TunnResult};
use boringtun::x25519::{PublicKey, StaticSecret};
use rand_core::OsRng;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int};
use std::slice;

pub const BT_ACTION_DONE: c_int = 0;
pub const BT_ACTION_WRITE_TO_NETWORK: c_int = 1;
pub const BT_ACTION_WRITE_TO_TUNNEL_IPV4: c_int = 4;
pub const BT_ACTION_WRITE_TO_TUNNEL_IPV6: c_int = 6;

pub struct BtTunnel {
    inner: Box<Tunn>,
}

fn write_c_str(s: &str, out: *mut c_char, cap: usize) -> bool {
    if out.is_null() || cap <= s.len() {
        return false;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(s.as_ptr(), out as *mut u8, s.len());
        *out.add(s.len()) = 0;
    }
    true
}

fn parse_key_b64(ptr: *const c_char) -> Option<[u8; 32]> {
    if ptr.is_null() {
        return None;
    }
    let cstr = unsafe { CStr::from_ptr(ptr) };
    let text = cstr.to_str().ok()?.trim();
    let decoded = B64.decode(text).ok()?;
    if decoded.len() != 32 {
        return None;
    }
    let mut key = [0u8; 32];
    key.copy_from_slice(&decoded);
    Some(key)
}

#[no_mangle]
pub extern "C" fn boringtun_generate_keypair(
    priv_b64_out: *mut c_char,
    priv_cap: usize,
    pub_b64_out: *mut c_char,
    pub_cap: usize,
) -> c_int {
    let secret = StaticSecret::random_from_rng(OsRng);
    let public = PublicKey::from(&secret);
    let priv_b64 = B64.encode(secret.to_bytes());
    let pub_b64 = B64.encode(public.as_bytes());
    if !write_c_str(&priv_b64, priv_b64_out, priv_cap)
        || !write_c_str(&pub_b64, pub_b64_out, pub_cap)
    {
        return -1;
    }
    0
}

#[no_mangle]
pub extern "C" fn boringtun_tunnel_new(
    priv_b64: *const c_char,
    server_pub_b64: *const c_char,
    keepalive_sec: u16,
) -> *mut BtTunnel {
    let Some(priv_bytes) = parse_key_b64(priv_b64) else {
        return std::ptr::null_mut();
    };
    let Some(pub_bytes) = parse_key_b64(server_pub_b64) else {
        return std::ptr::null_mut();
    };
    let secret = StaticSecret::from(priv_bytes);
    let peer_pub = PublicKey::from(pub_bytes);
    let keepalive = if keepalive_sec == 0 {
        None
    } else {
        Some(keepalive_sec)
    };
    let tunn = Tunn::new(secret, peer_pub, None, keepalive, 0, None);
    Box::into_raw(Box::new(BtTunnel { inner: Box::new(tunn) }))
}

#[no_mangle]
pub extern "C" fn boringtun_tunnel_free(tun: *mut BtTunnel) {
    if !tun.is_null() {
        unsafe {
            drop(Box::from_raw(tun));
        }
    }
}

fn map_result(res: TunnResult<'_>, out_action: Option<&mut c_int>) -> c_int {
    match res {
        TunnResult::Done => {
            if let Some(a) = out_action {
                *a = BT_ACTION_DONE;
            }
            0
        }
        TunnResult::Err(_) => -1,
        TunnResult::WriteToNetwork(buf) => {
            if let Some(a) = out_action {
                *a = BT_ACTION_WRITE_TO_NETWORK;
            }
            buf.len() as c_int
        }
        TunnResult::WriteToTunnelV4(buf, _) => {
            if let Some(a) = out_action {
                *a = BT_ACTION_WRITE_TO_TUNNEL_IPV4;
            }
            buf.len() as c_int
        }
        TunnResult::WriteToTunnelV6(buf, _) => {
            if let Some(a) = out_action {
                *a = BT_ACTION_WRITE_TO_TUNNEL_IPV6;
            }
            buf.len() as c_int
        }
    }
}

#[no_mangle]
pub extern "C" fn boringtun_encapsulate(
    tun: *mut BtTunnel,
    src: *const u8,
    src_len: usize,
    dst: *mut u8,
    dst_cap: usize,
) -> c_int {
    if tun.is_null() || src.is_null() || dst.is_null() {
        return -1;
    }
    let tun = unsafe { &mut *tun };
    let src_slice = unsafe { slice::from_raw_parts(src, src_len) };
    let dst_slice = unsafe { slice::from_raw_parts_mut(dst, dst_cap) };
    map_result(tun.inner.encapsulate(src_slice, dst_slice), None)
}

#[no_mangle]
pub extern "C" fn boringtun_decapsulate(
    tun: *mut BtTunnel,
    src: *const u8,
    src_len: usize,
    dst: *mut u8,
    dst_cap: usize,
    out_action: *mut c_int,
) -> c_int {
    if tun.is_null() || src.is_null() || dst.is_null() {
        return -1;
    }
    let tun = unsafe { &mut *tun };
    let src_slice = unsafe { slice::from_raw_parts(src, src_len) };
    let dst_slice = unsafe { slice::from_raw_parts_mut(dst, dst_cap) };
    let action_ref = if out_action.is_null() {
        None
    } else {
        Some(unsafe { &mut *out_action })
    };
    map_result(tun.inner.decapsulate(None, src_slice, dst_slice), action_ref)
}

#[no_mangle]
pub extern "C" fn boringtun_update_timers(
    tun: *mut BtTunnel,
    dst: *mut u8,
    dst_cap: usize,
) -> c_int {
    if tun.is_null() || dst.is_null() {
        return -1;
    }
    let tun = unsafe { &mut *tun };
    let dst_slice = unsafe { slice::from_raw_parts_mut(dst, dst_cap) };
    map_result(tun.inner.update_timers(dst_slice), None)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    #[test]
    fn handshake_and_packet_roundtrip() {
        let mut priv_a = [0 as c_char; 64];
        let mut pub_a = [0 as c_char; 64];
        let mut priv_b = [0 as c_char; 64];
        let mut pub_b = [0 as c_char; 64];
        assert_eq!(
            boringtun_generate_keypair(priv_a.as_mut_ptr(), 64, pub_a.as_mut_ptr(), 64),
            0
        );
        assert_eq!(
            boringtun_generate_keypair(priv_b.as_mut_ptr(), 64, pub_b.as_mut_ptr(), 64),
            0
        );

        let tun_a = boringtun_tunnel_new(priv_a.as_ptr(), pub_b.as_ptr(), 25);
        let tun_b = boringtun_tunnel_new(priv_b.as_ptr(), pub_a.as_ptr(), 25);
        assert!(!tun_a.is_null() && !tun_b.is_null());

        let mut net_buf = [0u8; 2048];
        let mut tun_buf = [0u8; 2048];
        let mut action: c_int = 0;

        let mut pkt = [0u8; 24];
        pkt[0] = 0x45;
        pkt[3] = 24;
        pkt[9] = 17;
        pkt[12..16].copy_from_slice(&[10, 66, 0, 2]);
        pkt[16..20].copy_from_slice(&[10, 66, 0, 1]);
        pkt[20..24].copy_from_slice(b"PING");

        let n = boringtun_encapsulate(tun_a, pkt.as_ptr(), pkt.len(), net_buf.as_mut_ptr(), net_buf.len());
        assert!(n > 0);
        let r = boringtun_decapsulate(
            tun_b,
            net_buf.as_ptr(),
            n as usize,
            tun_buf.as_mut_ptr(),
            tun_buf.len(),
            &mut action,
        );
        assert!(r > 0);
        assert_eq!(action, BT_ACTION_WRITE_TO_NETWORK);

        let r2 = boringtun_decapsulate(
            tun_a,
            tun_buf.as_ptr(),
            r as usize,
            net_buf.as_mut_ptr(),
            net_buf.len(),
            &mut action,
        );
        assert!(r2 >= 0);

        let bad = CString::new("invalid").unwrap();
        assert!(boringtun_tunnel_new(bad.as_ptr(), pub_b.as_ptr(), 0).is_null());

        boringtun_tunnel_free(tun_a);
        boringtun_tunnel_free(tun_b);
    }
}
