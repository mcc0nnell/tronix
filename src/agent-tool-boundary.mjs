// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Experimental fixed-capability bridge for model-driven tool callers.
// No shell, URLs, dynamic operation selection, or caller-selected identity.
import {randomUUID} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';

const TOOL = 'tronix_demo_constant';
const OPERATION = 'demo.constant';
const AGENT = 'tool-bridge-fixture';
const DEADLINE_MS = 2000;
const TERMINAL = new Set(['completed', 'failed', 'expired', 'overrun', 'interrupted']);

// This guard is intentionally at the application boundary, not a sandbox.
// The trusted operator must separately restrict access to tronixd and all
// other OpenClaw tools (including generic exec and elevated host access).
export function createRestrictedToolHandler(client) {
  if (!client || typeof client.submit !== 'function' ||
      typeof client.result !== 'function' || typeof client.ack !== 'function') {
    throw new TypeError('Tronix client required');
  }
  return async function handle(input) {
    if (input === null || typeof input !== 'object' ||
        Object.getPrototypeOf(input) !== Object.prototype ||
        Reflect.ownKeys(input).length !== 1 ||
        !Object.prototype.hasOwnProperty.call(input, 'tool')) {
      throw new Error('DENIED: strict tool request required');
    }
    const descriptor = Object.getOwnPropertyDescriptor(input, 'tool');
    if (!descriptor || !Object.prototype.hasOwnProperty.call(descriptor, 'value') ||
        descriptor.value !== TOOL) {
      throw new Error('DENIED: unknown tool');
    }

    // The model controls only the choice of a published, static tool.
    // Identifier, policy identity, operation, and deadline are not provided by it.
    const id = randomUUID();
    const admission = await client.submit(id, AGENT, OPERATION, DEADLINE_MS);
    if (!admission || admission.ok !== true) {
      return {ok: false, state: 'denied', id, error: 'NOT_ADMITTED'};
    }

    // Observation is bounded. No auto-retry or assertion of cancellation.
    for (let attempt = 0; attempt < 80; attempt++) {
      const observation = await client.result(id);
      if (observation?.ok === true && TERMINAL.has(observation.state)) {
        const state = observation.state;
        const response = state === 'completed'
          ? {ok: true, state, id, value: observation.value}
          : {ok: false, state, id};
        // The durable ledger can retain the receipt even after ack.
        await client.ack(id);
        return response;
      }
      if (!observation || observation.ok !== true ||
          !['queued', 'running'].includes(observation.state)) {
        return {ok: false, state: 'unknown', id};
      }
      await delay(20);
    }
    // The job may still be running or may have had side effects. Never retry.
    return {ok: false, state: 'unknown', id};
  };
}

export const allowedToolName = TOOL;
