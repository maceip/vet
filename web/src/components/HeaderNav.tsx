import "./HeaderNav.css";

export default function HeaderNav() {
  return (
    <header className="siteNav" aria-label="Site navigation">
      <a className="siteNavBrand" href="/TiHKAL/" aria-label="Replayable Systems home">
        <BrandMark />
        <div className="siteNavBrandText">
          <strong>REPLAYABLE</strong>
          <span>SYSTEMS</span>
        </div>
      </a>

      <a
        className="siteNavGh"
        href="https://github.com/maceip/TiHKAL"
        target="_blank"
        rel="noopener noreferrer"
        aria-label="View source on GitHub"
      >
        <GithubMark />
      </a>
    </header>
  );
}

function BrandMark() {
  return (
    <svg
      viewBox="0 0 32 32"
      className="brandMark"
      aria-hidden="true"
      role="img"
    >
      <rect x="3" y="6" width="11" height="5" rx="1" fill="currentColor" opacity="0.6" />
      <rect x="14" y="14" width="6" height="6" fill="currentColor" />
      <rect x="20" y="6" width="3" height="11" rx="1" fill="currentColor" opacity="0.6" />
      <rect x="9" y="22" width="14" height="5" rx="1" fill="currentColor" opacity="0.6" />
    </svg>
  );
}

function GithubMark() {
  return (
    <svg
      viewBox="0 0 24 24"
      className="ghMark"
      aria-hidden="true"
      role="img"
    >
      <path
        fill="currentColor"
        d="M12 .5C5.65.5.5 5.65.5 12c0 5.08 3.29 9.39 7.86 10.91.58.1.79-.25.79-.56v-2c-3.2.7-3.87-1.36-3.87-1.36-.52-1.31-1.27-1.66-1.27-1.66-1.04-.71.08-.7.08-.7 1.15.08 1.76 1.18 1.76 1.18 1.02 1.76 2.69 1.25 3.35.96.1-.74.4-1.25.72-1.54-2.55-.29-5.24-1.27-5.24-5.66 0-1.25.45-2.27 1.18-3.07-.12-.29-.51-1.46.11-3.04 0 0 .96-.31 3.15 1.17.91-.25 1.89-.38 2.86-.38.97 0 1.95.13 2.86.38 2.18-1.48 3.14-1.17 3.14-1.17.62 1.58.23 2.75.11 3.04.74.8 1.18 1.82 1.18 3.07 0 4.4-2.69 5.36-5.25 5.65.41.36.78 1.07.78 2.16v3.2c0 .31.21.67.8.56C20.21 21.39 23.5 17.08 23.5 12 23.5 5.65 18.35.5 12 .5z"
      />
    </svg>
  );
}
