#pragma once
// Umbrella header for the transport/rest submodule. Prefer rest/fwd.hpp when a
// declaration suffices, or the single narrow header a caller actually needs -
// `rest/response.hpp` alone is enough to inspect a result, and costs no Asio.
//
//   method.hpp    the verbs, and whether re-sending one is safe
//   request.hpp   what to send: verb, target, headers, body
//   response.hpp  what came back, or `failure` saying why not
//   options.hpp   how it is carried: certificate policy and deadline
//   client.hpp    one request, one connection
//   pipeline.hpp  many requests, one connection, one handshake
//
// IWYU pragma: begin_exports
#include "transport/rest/client.hpp"
#include "transport/rest/method.hpp"
#include "transport/rest/options.hpp"
#include "transport/rest/pipeline.hpp"
#include "transport/rest/request.hpp"
#include "transport/rest/response.hpp"
// IWYU pragma: end_exports
