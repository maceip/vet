import { useEffect, useState } from "react";
import "./CompactHeader.css";

/**
 * Sticky compact title bar that docks above the cube area as the user
 * scrolls past the big hero title. Hidden at the top of the page;
 * slides in once the user is past ~scrollThreshold pixels.
 */
export default function CompactHeader() {
  const [pinned, setPinned] = useState(false);

  useEffect(() => {
    const threshold = 260;
    const onScroll = () => {
      setPinned(window.scrollY > threshold);
    };
    onScroll();
    window.addEventListener("scroll", onScroll, { passive: true });
    return () => window.removeEventListener("scroll", onScroll);
  }, []);

  return (
    <div
      className={`compactHeader ${pinned ? "is-pinned" : ""}`}
      role="presentation"
      aria-hidden={!pinned}
    >
      <span className="compactHeader__replayable">REPLAYABLE</span>
      <span className="compactHeader__agent">AGENT MEMORY</span>
    </div>
  );
}
