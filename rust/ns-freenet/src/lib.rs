//! Northstar — Freenet client-protocol encoding, over freenet-stdlib.
//!
//! The browser owns the WebSocket transport (libcurl, already linked). This
//! crate owns only the wire format, so the node's `ClientRequest` and
//! `HostResponse` types are the upstream ones rather than a hand-built copy.

use std::os::raw::c_char;

use freenet_stdlib::client_api::{
    ClientRequest, HostResponse, NodeDiagnosticsConfig, NodeQuery, QueryResponse,
};

/// Encode the "which contracts does this node know" request.
///
/// Every `include_*` flag is off and `contract_keys` is empty, which the node
/// reads as "all contract ids, nothing else".
///
/// Returns a malloc'd buffer; free it with `ns_freenet_rs_free`.
#[no_mangle]
pub extern "C" fn ns_freenet_rs_contract_query(out_len: *mut usize) -> *mut u8 {
    if out_len.is_null() {
        return std::ptr::null_mut();
    }
    let config = NodeDiagnosticsConfig {
        include_node_info: false,
        include_network_info: false,
        include_subscriptions: true,
        contract_keys: Vec::new(),
        include_system_metrics: false,
        include_detailed_peer_info: false,
        include_subscriber_peer_ids: false,
    };
    let request = ClientRequest::NodeQueries(NodeQuery::NodeDiagnostics { config });

    let bytes = match bincode::serialize(&request) {
        Ok(b) => b,
        Err(_) => return std::ptr::null_mut(),
    };
    unsafe { *out_len = bytes.len() };
    let mut boxed = bytes.into_boxed_slice();
    let ptr = boxed.as_mut_ptr();
    std::mem::forget(boxed);
    ptr
}

/// Decode a node reply and return its contract ids, one per line.
///
/// Returns NUL-terminated UTF-8, or NULL when the reply is not a diagnostics
/// response. Free with `ns_freenet_rs_free_string`.
#[no_mangle]
pub extern "C" fn ns_freenet_rs_contract_ids(data: *const u8, len: usize) -> *mut c_char {
    if data.is_null() || len == 0 {
        return std::ptr::null_mut();
    }
    let bytes = unsafe { std::slice::from_raw_parts(data, len) };

    let response: HostResponse = match bincode::deserialize(bytes) {
        Ok(r) => r,
        Err(_) => return std::ptr::null_mut(),
    };
    let diagnostics = match response {
        HostResponse::QueryResponse(QueryResponse::NodeDiagnostics(d)) => d,
        _ => return std::ptr::null_mut(),
    };

    let mut ids: Vec<String> = diagnostics.contract_states.keys().cloned().collect();
    for subscription in &diagnostics.subscriptions {
        ids.push(subscription.contract_key.to_string());
    }
    ids.sort();
    ids.dedup();

    let joined = ids.join("\n");
    match std::ffi::CString::new(joined) {
        Ok(s) => s.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub extern "C" fn ns_freenet_rs_free(ptr: *mut u8, len: usize) {
    if ptr.is_null() {
        return;
    }
    unsafe {
        drop(Box::from_raw(std::slice::from_raw_parts_mut(ptr, len)));
    }
}

#[no_mangle]
pub extern "C" fn ns_freenet_rs_free_string(ptr: *mut c_char) {
    if ptr.is_null() {
        return;
    }
    unsafe {
        drop(std::ffi::CString::from_raw(ptr));
    }
}
