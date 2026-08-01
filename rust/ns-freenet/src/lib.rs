//! Northstar — Freenet client-protocol encoding, over freenet-stdlib.
//!
//! The browser owns the WebSocket transport (libcurl, already linked). This
//! crate owns only the wire format, so the node's `ClientRequest` and
//! `HostResponse` types are the upstream ones rather than a hand-built copy.

use std::os::raw::c_char;

use freenet_stdlib::client_api::ClientError;
use freenet_stdlib::client_api::{
    ClientRequest, HostResponse, NodeDiagnosticsConfig, NodeDiagnosticsResponse, NodeQuery,
    QueryResponse,
};

fn encode_diagnostics(out_len: *mut usize, node_and_network: bool) -> *mut u8 {
    if out_len.is_null() {
        return std::ptr::null_mut();
    }
    let config = NodeDiagnosticsConfig {
        include_node_info: node_and_network,
        include_network_info: node_and_network,
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

/// Encode the "which contracts does this node know" request.
///
/// Every `include_*` flag is off and `contract_keys` is empty, which the node
/// reads as "all contract ids, nothing else".
///
/// Returns a malloc'd buffer; free it with `ns_freenet_rs_free`.
#[no_mangle]
pub extern "C" fn ns_freenet_rs_contract_query(out_len: *mut usize) -> *mut u8 {
    encode_diagnostics(out_len, false)
}

/// Encode the same request with the node's own identity and its peer list
/// added, which is what the toolbar indicator and the console report.
#[no_mangle]
pub extern "C" fn ns_freenet_rs_status_query(out_len: *mut usize) -> *mut u8 {
    encode_diagnostics(out_len, true)
}

/// Decode a node reply into `key=value` lines the browser can read without
/// knowing the Rust types: peers, connections, contracts, uptime, gateway,
/// peer_id.
///
/// Returns NUL-terminated UTF-8, or NULL when the reply is not a diagnostics
/// response. Free with `ns_freenet_rs_free_string`.
#[no_mangle]
pub extern "C" fn ns_freenet_rs_status_text(data: *const u8, len: usize) -> *mut c_char {
    let Some(diagnostics) = decode_diagnostics(data, len) else {
        return std::ptr::null_mut();
    };

    let mut lines = Vec::new();
    if let Some(network) = &diagnostics.network_info {
        lines.push(format!("peers={}", network.connected_peers.len()));
        lines.push(format!("connections={}", network.active_connections));
    }
    lines.push(format!("contracts={}", diagnostics.contract_states.len()));
    if let Some(node) = &diagnostics.node_info {
        lines.push(format!("uptime={}", node.uptime_seconds));
        lines.push(format!("gateway={}", if node.is_gateway { 1 } else { 0 }));
        if !node.peer_id.is_empty() {
            lines.push(format!("peer_id={}", node.peer_id.replace('\n', " ")));
        }
    }

    match std::ffi::CString::new(lines.join("\n")) {
        Ok(s) => s.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

fn decode_diagnostics(data: *const u8, len: usize) -> Option<NodeDiagnosticsResponse> {
    if data.is_null() || len == 0 {
        return None;
    }
    let bytes = unsafe { std::slice::from_raw_parts(data, len) };

    // The node answers with the client API's HostResult, which is a
    // Result<HostResponse, ClientError> — the bare HostResponse is one
    // discriminant short and will not deserialize.
    let response = match bincode::deserialize::<Result<HostResponse, ClientError>>(bytes) {
        Ok(Ok(r)) => r,
        Ok(Err(_)) => return None,
        Err(_) => match bincode::deserialize::<HostResponse>(bytes) {
            Ok(r) => r,
            Err(_) => return None,
        },
    };
    match response {
        HostResponse::QueryResponse(QueryResponse::NodeDiagnostics(d)) => Some(d),
        _ => None,
    }
}

/// Decode a node reply and return its contract ids, one per line.
///
/// Returns NUL-terminated UTF-8, or NULL when the reply is not a diagnostics
/// response. Free with `ns_freenet_rs_free_string`.
#[no_mangle]
pub extern "C" fn ns_freenet_rs_contract_ids(data: *const u8, len: usize) -> *mut c_char {
    let Some(diagnostics) = decode_diagnostics(data, len) else {
        return std::ptr::null_mut();
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
