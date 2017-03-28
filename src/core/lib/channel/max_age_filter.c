//
// Copyright 2017, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include "src/core/lib/channel/message_size_filter.h"

#include <limits.h>
#include <string.h>

#include <grpc/support/log.h>
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/transport/http2_errors.h"
#include "src/core/lib/transport/service_config.h"

#define DEFAULT_MAX_CONNECTION_AGE_S INT_MAX
#define DEFAULT_MAX_CONNECTION_AGE_GRACE_S INT_MAX

typedef struct channel_data {
  // We take a reference to the channel stack for the timer callback
  grpc_channel_stack* channel_stack;
  // Guards access to max_age_timer and max_age_timer_pending
  gpr_mu max_age_timer_mu;
  // True if the max_age timer callback is currently pending
  bool max_age_timer_pending;
  // True if the max_age timer callback is currently pending
  bool max_age_grace_timer_pending;
  // The timer for checking if the channel has reached its max age
  grpc_timer max_age_timer;
  // The timer for checking if the channel has reached its max age
  grpc_timer max_age_grace_timer;
  // Allowed max time a channel may exist
  gpr_timespec max_connection_age;
  // Allowed grace period after the channel reaches its max age
  gpr_timespec max_connection_age_grace;
  // Closure to run when the channel reaches its max age and should be closed
  // gracefully
  grpc_closure close_max_age_channel;
  // Closure to run the channel uses up its max age grace time and should be
  // closed forcibly
  grpc_closure force_close_max_age_channel;
  // Closure to run when the init fo channel stack is done and the max_age timer
  // should be started
  grpc_closure start_max_age_timer_after_init;
  // Closure to run when the goaway op is finished and the max_age_timer
  grpc_closure start_max_age_grace_timer_after_goaway_op;
} channel_data;

static void start_max_age_timer_after_init(grpc_exec_ctx* exec_ctx, void* arg,
                                           grpc_error* error) {
  channel_data* chand = arg;
  gpr_mu_lock(&chand->max_age_timer_mu);
  chand->max_age_timer_pending = true;
  grpc_timer_init(
      exec_ctx, &chand->max_age_timer,
      gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), chand->max_connection_age),
      &chand->close_max_age_channel, gpr_now(GPR_CLOCK_MONOTONIC));
  gpr_mu_unlock(&chand->max_age_timer_mu);
  GRPC_CHANNEL_STACK_UNREF(exec_ctx, chand->channel_stack,
                           "max_age start_max_age_timer_after_init");
}

static void start_max_age_grace_timer_after_goaway_op(grpc_exec_ctx* exec_ctx,
                                                      void* arg,
                                                      grpc_error* error) {
  channel_data* chand = arg;
  gpr_mu_lock(&chand->max_age_timer_mu);
  chand->max_age_grace_timer_pending = true;
  grpc_timer_init(exec_ctx, &chand->max_age_grace_timer,
                  gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC),
                               chand->max_connection_age_grace),
                  &chand->force_close_max_age_channel,
                  gpr_now(GPR_CLOCK_MONOTONIC));
  gpr_mu_unlock(&chand->max_age_timer_mu);
  GRPC_CHANNEL_STACK_UNREF(exec_ctx, chand->channel_stack,
                           "max_age start_max_age_grace_timer_after_goaway_op");
}

static void close_max_age_channel(grpc_exec_ctx* exec_ctx, void* arg,
                                  grpc_error* error) {
  channel_data* chand = arg;
  gpr_mu_lock(&chand->max_age_timer_mu);
  chand->max_age_timer_pending = false;
  gpr_mu_unlock(&chand->max_age_timer_mu);
  if (error == GRPC_ERROR_NONE) {
    GRPC_CHANNEL_STACK_REF(chand->channel_stack,
                           "max_age start_max_age_grace_timer_after_goaway_op");
    grpc_transport_op* op = grpc_make_transport_op(
        &chand->start_max_age_grace_timer_after_goaway_op);
    op->goaway_error =
        grpc_error_set_int(GRPC_ERROR_CREATE_FROM_STATIC_STRING("max_age"),
                           GRPC_ERROR_INT_HTTP2_ERROR, GRPC_HTTP2_NO_ERROR);
    grpc_channel_element* elem =
        grpc_channel_stack_element(chand->channel_stack, 0);
    elem->filter->start_transport_op(exec_ctx, elem, op);
  } else if (error != GRPC_ERROR_CANCELLED) {
    GRPC_LOG_IF_ERROR("close_max_age_channel", error);
  }
}

static void force_close_max_age_channel(grpc_exec_ctx* exec_ctx, void* arg,
                                        grpc_error* error) {
  channel_data* chand = arg;
  gpr_mu_lock(&chand->max_age_timer_mu);
  chand->max_age_grace_timer_pending = false;
  gpr_mu_unlock(&chand->max_age_timer_mu);
  if (error == GRPC_ERROR_NONE) {
    grpc_transport_op* op = grpc_make_transport_op(NULL);
    op->disconnect_with_error =
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("Channel reaches max age");
    grpc_channel_element* elem =
        grpc_channel_stack_element(chand->channel_stack, 0);
    elem->filter->start_transport_op(exec_ctx, elem, op);
  } else if (error != GRPC_ERROR_CANCELLED) {
    GRPC_LOG_IF_ERROR("force_close_max_age_channel", error);
  }
}

// Constructor for call_data.
static grpc_error* init_call_elem(grpc_exec_ctx* exec_ctx,
                                  grpc_call_element* elem,
                                  const grpc_call_element_args* args) {
  // call_num ++;
  return GRPC_ERROR_NONE;
}

// Destructor for call_data.
static void destroy_call_elem(grpc_exec_ctx* exec_ctx, grpc_call_element* elem,
                              const grpc_call_final_info* final_info,
                              grpc_closure* ignored) {
  // call_num --;
}

// Constructor for channel_data.
static grpc_error* init_channel_elem(grpc_exec_ctx* exec_ctx,
                                     grpc_channel_element* elem,
                                     grpc_channel_element_args* args) {
  channel_data* chand = elem->channel_data;
  gpr_mu_init(&chand->max_age_timer_mu);
  chand->max_age_timer_pending = false;
  chand->max_age_grace_timer_pending = false;
  chand->channel_stack = args->channel_stack;
  chand->max_connection_age =
      DEFAULT_MAX_CONNECTION_AGE_S == INT_MAX
          ? gpr_inf_future(GPR_TIMESPAN)
          : gpr_time_from_seconds(DEFAULT_MAX_CONNECTION_AGE_S, GPR_TIMESPAN);
  chand->max_connection_age =
      DEFAULT_MAX_CONNECTION_AGE_GRACE_S == INT_MAX
          ? gpr_inf_future(GPR_TIMESPAN)
          : gpr_time_from_seconds(DEFAULT_MAX_CONNECTION_AGE_GRACE_S,
                                  GPR_TIMESPAN);
  for (size_t i = 0; i < args->channel_args->num_args; ++i) {
    if (0 ==
        strcmp(args->channel_args->args[i].key, GPRC_ARG_MAX_CONNECION_AGE_S)) {
      const int value = grpc_channel_arg_get_integer(
          &args->channel_args->args[i],
          (grpc_integer_options){DEFAULT_MAX_CONNECTION_AGE_S, 1, INT_MAX});
      chand->max_connection_age =
          value == INT_MAX ? gpr_inf_future(GPR_TIMESPAN)
                           : gpr_time_from_seconds(value, GPR_TIMESPAN);
    } else if (0 == strcmp(args->channel_args->args[i].key,
                           GPRC_ARG_MAX_CONNECION_AGE_GRACE_S)) {
      const int value = grpc_channel_arg_get_integer(
          &args->channel_args->args[i],
          (grpc_integer_options){DEFAULT_MAX_CONNECTION_AGE_GRACE_S, 1,
                                 INT_MAX});
      chand->max_connection_age_grace =
          value == INT_MAX ? gpr_inf_future(GPR_TIMESPAN)
                           : gpr_time_from_seconds(value, GPR_TIMESPAN);
    }
  }
  grpc_closure_init(&chand->close_max_age_channel, close_max_age_channel, chand,
                    grpc_schedule_on_exec_ctx);
  grpc_closure_init(&chand->force_close_max_age_channel,
                    force_close_max_age_channel, chand,
                    grpc_schedule_on_exec_ctx);
  grpc_closure_init(&chand->start_max_age_timer_after_init,
                    start_max_age_timer_after_init, chand,
                    grpc_schedule_on_exec_ctx);
  grpc_closure_init(&chand->start_max_age_grace_timer_after_goaway_op,
                    start_max_age_grace_timer_after_goaway_op, chand,
                    grpc_schedule_on_exec_ctx);

  if (gpr_time_cmp(chand->max_connection_age, gpr_inf_future(GPR_TIMESPAN)) !=
      0) {
    // When the channel reaches its max age, we send down an op with
    // goaway_error set.  However, we can't send down any ops until after the
    // channel stack is fully initialized.  If we start the timer here, we have
    // no guarantee that the timer won't pop before channel stack initialization
    // is finished.  To avoid that problem, we create a closure to start the
    // timer, and we schedule that closure to be run after call stack
    // initialization is done.
    GRPC_CHANNEL_STACK_REF(chand->channel_stack,
                           "max_age start_max_age_timer_after_init");
    grpc_closure_sched(exec_ctx, &chand->start_max_age_timer_after_init,
                       GRPC_ERROR_NONE);
  }

  return GRPC_ERROR_NONE;
}

// Destructor for channel_data.
static void destroy_channel_elem(grpc_exec_ctx* exec_ctx,
                                 grpc_channel_element* elem) {
  channel_data* chand = elem->channel_data;
  gpr_mu_lock(&chand->max_age_timer_mu);
  if (chand->max_age_timer_pending) {
    grpc_timer_cancel(exec_ctx, &chand->max_age_timer);
  }
  if (chand->max_age_grace_timer_pending) {
    grpc_timer_cancel(exec_ctx, &chand->max_age_grace_timer);
  }
  gpr_mu_unlock(&chand->max_age_timer_mu);
}

const grpc_channel_filter grpc_max_age_filter = {
    grpc_call_next_op,
    grpc_channel_next_op,
    0,  // sizeof_call_data
    init_call_elem,
    grpc_call_stack_ignore_set_pollset_or_pollset_set,
    destroy_call_elem,
    sizeof(channel_data),
    init_channel_elem,
    destroy_channel_elem,
    grpc_call_next_get_peer,
    grpc_channel_next_get_info,
    "max_age"};
