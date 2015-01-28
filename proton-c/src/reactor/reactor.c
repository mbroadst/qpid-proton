/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include <proton/object.h>
#include <proton/handlers.h>
#include <proton/io.h>
#include <proton/event.h>
#include <proton/transport.h>
#include <proton/connection.h>
#include <proton/session.h>
#include <proton/link.h>
#include <proton/delivery.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "reactor.h"
#include "selectable.h"
#include "platform.h"

struct pn_reactor_t {
  pn_record_t *attachments;
  pn_io_t *io;
  pn_collector_t *collector;
  pn_handler_t *global;
  pn_handler_t *handler;
  pn_list_t *children;
  pn_timer_t *timer;
  pn_selectable_t *selectable;
  pn_event_type_t previous;
  pn_timestamp_t now;
  int selectables;
  int timeout;
  bool yield;
};

void pn_reactor_mark(pn_reactor_t *reactor) {
  assert(reactor);
  reactor->now = pn_i_now();
}

static void pn_reactor_initialize(pn_reactor_t *reactor) {
  reactor->attachments = pn_record();
  reactor->io = pn_io();
  reactor->collector = pn_collector();
  reactor->global = pn_iohandler();
  reactor->handler = pn_handler(NULL);
  reactor->children = pn_list(PN_OBJECT, 0);
  reactor->timer = pn_timer(reactor->collector);
  reactor->selectable = NULL;
  reactor->previous = PN_EVENT_NONE;
  reactor->selectables = 0;
  reactor->timeout = 0;
  reactor->yield = false;
  pn_reactor_mark(reactor);
}

static void pn_reactor_finalize(pn_reactor_t *reactor) {
  pn_decref(reactor->attachments);
  pn_decref(reactor->collector);
  pn_decref(reactor->global);
  pn_decref(reactor->handler);
  pn_decref(reactor->children);
  pn_decref(reactor->timer);
  pn_decref(reactor->io);
}

#define pn_reactor_hashcode NULL
#define pn_reactor_compare NULL
#define pn_reactor_inspect NULL

PN_CLASSDEF(pn_reactor)

pn_reactor_t *pn_reactor() {
  return pn_reactor_new();
}

pn_record_t *pn_reactor_attachments(pn_reactor_t *reactor) {
  assert(reactor);
  return reactor->attachments;
}

int pn_reactor_timeout(pn_reactor_t *reactor) {
  assert(reactor);
  return reactor->timeout;
}

void pn_reactor_free(pn_reactor_t *reactor) {
  if (reactor) {
    pn_collector_release(reactor->collector);
    pn_handler_free(reactor->handler);
    reactor->handler = NULL;
    pn_decref(reactor);
  }
}

void pn_reactor_global(pn_reactor_t *reactor, pn_handler_t *handler) {
  assert(reactor);
  pn_decref(reactor->global);
  reactor->global = handler;
  pn_incref(reactor->global);
}

pn_handler_t *pn_reactor_handler(pn_reactor_t *reactor) {
  assert(reactor);
  return reactor->handler;
}

pn_io_t *pn_reactor_io(pn_reactor_t *reactor) {
  assert(reactor);
  return reactor->io;
}

pn_collector_t *pn_reactor_collector(pn_reactor_t *reactor) {
  assert(reactor);
  return reactor->collector;
}

pn_list_t *pn_reactor_children(pn_reactor_t *reactor) {
  assert(reactor);
  return reactor->children;
}

static void pni_selectable_release(pn_selectable_t *selectable) {
  pn_reactor_t *reactor = (pn_reactor_t *) pni_selectable_get_context(selectable);
  pn_incref(selectable);
  if (pn_list_remove(reactor->children, selectable)) {
    reactor->selectables--;
  }
  pn_decref(selectable);
}

pn_selectable_t *pn_reactor_selectable(pn_reactor_t *reactor) {
  assert(reactor);
  pn_selectable_t *sel = pn_selectable();
  pn_selectable_collect(sel, reactor->collector);
  pn_collector_put(reactor->collector, PN_OBJECT, sel, PN_SELECTABLE_INIT);
  pni_selectable_set_context(sel, reactor);
  pn_list_add(reactor->children, sel);
  pn_selectable_on_release(sel, pni_selectable_release);
  pn_decref(sel);
  reactor->selectables++;
  return sel;
}

PN_HANDLE(PNI_TERMINATED)

void pn_reactor_update(pn_reactor_t *reactor, pn_selectable_t *selectable) {
  assert(reactor);
  pn_record_t *record = pn_selectable_attachments(selectable);
  if (!pn_record_has(record, PNI_TERMINATED)) {
    if (pn_selectable_is_terminal(selectable)) {
      pn_record_def(record, PNI_TERMINATED, PN_VOID);
      pn_collector_put(reactor->collector, PN_OBJECT, selectable, PN_SELECTABLE_FINAL);
    } else {
      pn_collector_put(reactor->collector, PN_OBJECT, selectable, PN_SELECTABLE_UPDATED);
    }
  }
}

static void pni_reactor_dispatch_pre(pn_reactor_t *reactor, pn_event_t *event) {
  assert(reactor);
  assert(event);
  switch (pn_event_type(event)) {
  case PN_CONNECTION_INIT:
    pni_record_init_reactor(pn_connection_attachments(pn_event_connection(event)), reactor);
    break;
  default:
    break;
  }
}

void pni_handle_final(pn_reactor_t *reactor, pn_event_t *event);

static void pni_reactor_dispatch_post(pn_reactor_t *reactor, pn_event_t *event) {
  assert(reactor);
  assert(event);
  switch (pn_event_type(event)) {
  case PN_CONNECTION_FINAL:
    pni_handle_final(reactor, event);
    break;
  default:
    break;
  }
}

PN_HANDLE(PN_HANDLER)

pn_handler_t *pn_record_get_handler(pn_record_t *record) {
  assert(record);
  return (pn_handler_t *) pn_record_get(record, PN_HANDLER);
}

void pn_record_set_handler(pn_record_t *record, pn_handler_t *handler) {
  assert(record);
  pn_record_def(record, PN_HANDLER, PN_OBJECT);
  pn_record_set(record, PN_HANDLER, handler);
}

PN_HANDLE(PN_REACTOR)

pn_reactor_t *pni_record_get_reactor(pn_record_t *record) {
  return (pn_reactor_t *) pn_record_get(record, PN_REACTOR);
}

void pni_record_init_reactor(pn_record_t *record, pn_reactor_t *reactor) {
  pn_record_def(record, PN_REACTOR, PN_WEAKREF);
  pn_record_set(record, PN_REACTOR, reactor);
}

static pn_connection_t *pni_object_connection(const pn_class_t *clazz, void *object) {
  switch (pn_class_id(clazz)) {
  case CID_pn_delivery:
    return pn_session_connection(pn_link_session(pn_delivery_link((pn_delivery_t *) object)));
  case CID_pn_link:
    return pn_session_connection(pn_link_session((pn_link_t *) object));
  case CID_pn_session:
    return pn_session_connection((pn_session_t *) object);
  case CID_pn_connection:
    return (pn_connection_t *) object;
  case CID_pn_transport:
    return pn_transport_connection((pn_transport_t *) object);
  default:
    return NULL;
  }
}

static pn_reactor_t *pni_reactor(pn_selectable_t *sel) {
  return (pn_reactor_t *) pni_selectable_get_context(sel);
}

pn_reactor_t *pn_event_reactor(pn_event_t *event) {
  const pn_class_t *clazz = pn_event_class(event);
  void *context = pn_event_context(event);
  switch (pn_class_id(clazz)) {
  case CID_pn_reactor:
    return (pn_reactor_t *) context;
  case CID_pn_task:
    return pni_record_get_reactor(pn_task_attachments((pn_task_t *) context));
  case CID_pn_transport:
    return pni_record_get_reactor(pn_transport_attachments((pn_transport_t *) context));
  case CID_pn_delivery:
  case CID_pn_link:
  case CID_pn_session:
  case CID_pn_connection:
    {
      pn_connection_t *conn = pni_object_connection(pn_event_class(event), context);
      pn_record_t *record = pn_connection_attachments(conn);
      return pni_record_get_reactor(record);
    }
  case CID_pn_selectable:
    {
      pn_selectable_t *sel = (pn_selectable_t *) pn_event_context(event);
      return pni_reactor(sel);
    }
  default:
    return NULL;
  }
}

pn_handler_t *pn_event_handler(pn_event_t *event, pn_handler_t *default_handler) {
  pn_handler_t *handler = NULL;
  pn_link_t *link = pn_event_link(event);
  if (link) {
    handler = pn_record_get_handler(pn_link_attachments(link));
    if (handler) { return handler; }
  }
  pn_session_t *session = pn_event_session(event);
  if (session) {
    handler = pn_record_get_handler(pn_session_attachments(session));
    if (handler) { return handler; }
  }
  pn_connection_t *connection = pn_event_connection(event);
  if (connection) {
    handler = pn_record_get_handler(pn_connection_attachments(connection));
    if (handler) { return handler; }
  }
  switch (pn_class_id(pn_event_class(event))) {
  case CID_pn_task:
    handler = pn_record_get_handler(pn_task_attachments((pn_task_t *) pn_event_context(event)));
    if (handler) { return handler; }
    break;
  case CID_pn_selectable:
    handler = pn_record_get_handler(pn_selectable_attachments((pn_selectable_t *) pn_event_context(event)));
    if (handler) { return handler; }
    break;
  default:
    break;
  }
  return default_handler;
}

pn_task_t *pn_reactor_schedule(pn_reactor_t *reactor, int delay, pn_handler_t *handler) {
  pn_task_t *task = pn_timer_schedule(reactor->timer, reactor->now + delay);
  pn_record_t *record = pn_task_attachments(task);
  pni_record_init_reactor(record, reactor);
  pn_record_set_handler(record, handler);
  if (reactor->selectable) {
    pn_selectable_set_deadline(reactor->selectable, pn_timer_deadline(reactor->timer));
    pn_reactor_update(reactor, reactor->selectable);
  }
  return task;
}

void pni_event_print(pn_event_t *event) {
  pn_string_t *str = pn_string(NULL);
  pn_inspect(event, str);
  printf("%s\n", pn_string_get(str));
  pn_free(str);
}

bool pni_reactor_more(pn_reactor_t *reactor) {
  assert(reactor);
  return pn_timer_tasks(reactor->timer) || reactor->selectables > 1;
}

void pn_reactor_yield(pn_reactor_t *reactor) {
  assert(reactor);
  reactor->yield = true;
}

bool pn_reactor_process(pn_reactor_t *reactor) {
  assert(reactor);
  pn_reactor_mark(reactor);
  pn_event_type_t previous = PN_EVENT_NONE;
  while (true) {
    pn_event_t *event = pn_collector_peek(reactor->collector);
    //pni_event_print(event);
    if (event) {
      if (reactor->yield) {
        reactor->yield = false;
        return true;
      }
      reactor->yield = false;
      pni_reactor_dispatch_pre(reactor, event);
      pn_handler_t *handler = pn_event_handler(event, reactor->handler);
      pn_handler_dispatch(handler, event);
      pn_handler_dispatch(reactor->global, event);
      pni_reactor_dispatch_post(reactor, event);
      previous = reactor->previous = pn_event_type(event);
      pn_collector_pop(reactor->collector);
    } else {
      if (pni_reactor_more(reactor)) {
        if (previous != PN_REACTOR_QUIESCED && reactor->previous != PN_REACTOR_FINAL) {
          pn_collector_put(reactor->collector, PN_OBJECT, reactor, PN_REACTOR_QUIESCED);
        } else {
          return true;
        }
      } else {
        if (reactor->selectable) {
          pn_selectable_terminate(reactor->selectable);
          pn_reactor_update(reactor, reactor->selectable);
          reactor->selectable = NULL;
        } else {
          return false;
        }
      }
    }
  }
}

static void pni_timer_expired(pn_selectable_t *sel) {
  pn_reactor_t *reactor = pni_reactor(sel);
  pn_timer_tick(reactor->timer, reactor->now);
  pn_selectable_set_deadline(sel, pn_timer_deadline(reactor->timer));
  pn_reactor_update(reactor, sel);
}

pn_selectable_t *pni_timer_selectable(pn_reactor_t *reactor) {
  pn_selectable_t *sel = pn_reactor_selectable(reactor);
  pn_selectable_on_expired(sel, pni_timer_expired);
  pn_selectable_set_deadline(sel, pn_timer_deadline(reactor->timer));
  pn_reactor_update(reactor, sel);
  return sel;
}

void pn_reactor_start(pn_reactor_t *reactor) {
  assert(reactor);
  pn_collector_put(reactor->collector, PN_OBJECT, reactor, PN_REACTOR_INIT);
  reactor->selectable = pni_timer_selectable(reactor);
 }

bool pn_reactor_work(pn_reactor_t *reactor, int timeout) {
  assert(reactor);
  reactor->timeout = timeout;
  return pn_reactor_process(reactor);
}

void pn_reactor_stop(pn_reactor_t *reactor) {
  assert(reactor);
  pn_collector_put(reactor->collector, PN_OBJECT, reactor, PN_REACTOR_FINAL);
  pn_reactor_process(reactor);
  pn_collector_release(reactor->collector);
}

void pn_reactor_run(pn_reactor_t *reactor) {
  assert(reactor);
  pn_reactor_start(reactor);
  while (pn_reactor_work(reactor, 1000)) {}
  pn_reactor_stop(reactor);
}