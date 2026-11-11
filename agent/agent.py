"""xv6 AI agent: request -> planner -> validator -> dispatcher -> QEMU bridge -> xv6 -> answer."""
import argparse
import json
import sys

import config
from planner import InjectionTestPlanner, LLMPlanner, RuleBasedPlanner
from qemu_bridge import BridgeError, QemuBridge
from tools import ToolDispatcher
from validator import ValidationError


class Agent:
    def __init__(self, planner, dispatcher, max_iterations=config.MAX_ITERATIONS):
        self.planner, self.dispatcher, self.max_iterations = planner, dispatcher, max_iterations

    def handle(self, request):
        """Return (answer, steps); each step records the proposal and what happened to it."""
        history, steps = [], []
        for _ in range(self.max_iterations):
            decision = self.planner.next_step(request, history)
            if "final" in decision:
                return decision["final"], steps
            try:
                obs = self.dispatcher.dispatch(decision)
            except ValidationError as e:
                obs = {"rejected": True, "ok": False, "output": f"Rejected by validator: {e}"}
            except BridgeError as e:
                obs = {"ok": False, "output": f"xv6 communication error: {e}"}
            steps.append({"proposed": decision, **obs})
            history.append((decision, obs))
        return f"Stopped after {self.max_iterations} steps without an answer.", steps


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--planner", choices=["rules", "inject", "llm"], default="rules",
                        help="rules: deterministic; inject: prompt-injection test double; llm: Claude")
    parser.add_argument("--trace", action="store_true", help="print every step as JSON")
    args = parser.parse_args()

    dispatcher = ToolDispatcher(QemuBridge())
    if args.planner == "llm":
        from llm_client import LLMClient

        client = LLMClient()
        planner, dispatcher.summarizer = LLMPlanner(client), client.summarize
    else:
        planner = InjectionTestPlanner() if args.planner == "inject" else RuleBasedPlanner()
    agent = Agent(planner, dispatcher)

    print(f"xv6 AI Agent (planner: {args.planner}). Empty line or Ctrl-D quits.")
    try:
        while True:
            try:
                request = input("\nEnter your request:\n").strip()
            except EOFError:
                break
            if not request:
                break
            if not sys.stdin.isatty():
                print(request)
            answer, steps = agent.handle(request)
            for step in steps:
                if args.trace:
                    print("[trace]", json.dumps(step))
                elif step.get("rejected"):
                    print(f"[blocked] {json.dumps(step['proposed'])}: {step['output']}")
            print(f"Agent:\n{answer}")
    finally:
        dispatcher.bridge.close()


if __name__ == "__main__":
    main()
