import LeftRail from "./LeftRail";
import ReplayableSpinBlock from "./ReplayableSpinBlock";

export function Hero() {
  return (
    <main className="heroPage">
      <header className="heroTitle">
        <h1>REPLAYABLE</h1>
        <h2>AGENT MEMORY</h2>
        <p>A log-based substrate for auditable AI decisions.</p>
      </header>

      <section className="heroGrid">
        <aside className="leftRail">
          <LeftRail />
        </aside>

        <ReplayableSpinBlock />
      </section>
    </main>
  );
}
