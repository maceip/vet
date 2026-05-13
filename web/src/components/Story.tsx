import type { ReactNode } from "react";
import "./Story.css";

type StoryPanel = {
  title: string;
  body: ReactNode;
};

export const STORY_PANELS: StoryPanel[] = [
  {
    title: "Opening hook",
    body: (
      <>
        Today's AI agents rewrite their own memory every few turns. By hour two
        of a real session, the agent is acting on a copy of a copy of a copy of
        what you actually said. We measured one real session where this rewriting
        took 17 model calls to produce a memory that had already lost the user's
        first instruction. No framework on the market today can tell you which
        events produced the agent's last decision — the memory has been edited
        too many times to know.
      </>
    ),
  },
  {
    title: "Handoff to yourself",
    body: (
      <>
        You hand off your agent to yourself a dozen times a day. Close the
        laptop, walk into a meeting, come back tomorrow — every one of those is a
        handoff. Today, the agent you come back to isn't the one you left: it's
        been re-summarizing itself the entire time, or it's empty and you're
        starting over. A replayable substrate makes it free — when you return,
        the agent rebuilds the memory it had when you left, same context, same
        prior decisions, all auditable.
      </>
    ),
  },
  {
    title: "A category, not a feature",
    body: (
      <>
        "Store a thing, mutate it as new things arrive, lose the history" used
        to be how software stored everything. Then the rest of computing moved
        on: Git replaced editable files with append-only commits; Kafka replaced
        editable queues with append-only streams; CRDTs replaced editable state
        with append-only operations. Every category of software built in the last
        decade has replaced <em>mutate-in-place</em> with{" "}
        <em>append-only-and-derive</em> — except agent memory, which still
        rewrites itself every turn the way Word documents got saved in 1995.
        There is no technical reason for this; there are only inherited
        assumptions.
      </>
    ),
  },
  {
    title: "Decisions with receipts",
    body: (
      <>
        When the agent does something you didn't expect — approves a refund,
        hands off a file, refuses a request — you can ask "why?" and get a real
        answer: not a paraphrase, not a summary, but the exact events that
        produced that decision. Today, this is impossible. Every popular agent
        framework has rewritten its own memory so many times by the time the
        agent acts that there is no chain of custody to walk; the agent's last
        decision points to a string the last summarize call wrote, not to
        anything that actually happened. A replayable substrate keeps that chain
        intact: every memory the agent uses is content-addressed, signed against
        the events it was built from, and rejected at the runtime gate if the
        bytes don't match.
      </>
    ),
  },
  {
    title: "Where it loses",
    body: (
      <>
        A lossless compressor preserves everything — including the things you'd
        rather it didn't. On one session, the user was pressuring the agent to
        bypass policy. The replayable agent preserved the request word-for-word
        and proposed two forbidden tool calls; the rolling-summary agent did
        neither. The replayable substrate lost this case. Replayable memory is a
        compression layer, not a policy layer — putting policy in the prompt is
        still your job.
      </>
    ),
  },
];

type StoryProps = {
  activeIndex: number;
};

/**
 * Reading material below the cube. The carousel owns the active index so only
 * the paragraph paired with the visible cube face is shown.
 */
export default function Story({ activeIndex }: StoryProps) {
  const panel = STORY_PANELS[activeIndex] ?? STORY_PANELS[0];

  return (
    <section className="story" aria-label="Why replayable agent memory">
      <article className="story__section" aria-live="polite">
        <span className="story__eyebrow">Selected face {activeIndex + 1}</span>
        <h2>{panel.title}</h2>
        <p>{panel.body}</p>
      </article>
    </section>
  );
}
