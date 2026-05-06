import "./HeaderNav.css";

export default function HeaderNav() {
  return (
    <header className="siteNav" aria-label="Site navigation">
      <div className="siteNavBrand">
        <BrandMark />
        <div className="siteNavBrandText">
          <strong>REPLAYABLE</strong>
          <span>SYSTEMS</span>
        </div>
      </div>
      <nav className="siteNavLinks">
        <a href="#overview" className="isActive">Overview</a>
        <a href="#how-it-works">How It Works</a>
        <a href="#docs">Docs</a>
        <a href="#research">Research</a>
        <a href="#about">About</a>
      </nav>
      <a href="#get-started" className="siteNavCta">Get Started</a>
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
