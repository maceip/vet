# Cognito Managed Login Branding Handoff

This page is not served from this repository. It is the AWS Cognito-hosted login
screen for the DPM benchmark experience.

## Target URL

Student-facing login URL:

```text
https://dpm-bench-095713295645.auth.eu-north-1.amazoncognito.com/login?client_id=5hdtmlm6o0jq2nq2dl31jie84o&response_type=code&redirect_uri=http://localhost:53682/callback&scope=openid+email
```

AWS location:

```text
AWS Console -> Cognito -> User Pools -> dpm-bench
Region: eu-north-1
Pool id: eu-north-1_p5gQLyNUG
```

Look under:

```text
App integration -> Domain -> Edit branding
```

AWS UI labels move around. The same controls may appear under "Branding" or
"Managed login".

## Preferred product path

Use **Managed Login Branding** if available.

- Classic Hosted UI is currently what the URL serves.
- Classic customization is limited and tends to look dated.
- Managed Login Branding is newer, supports passkeys natively, and has better
  styling/preview controls.
- Managed Login requires `ManagedLoginVersion: 2` on the user pool domain.

If Managed Login is not enabled yet, ask the AWS owner to enable the version 2
managed login domain before spending time on deep polish.

## Design direction

Match the selected benchmark direction:

```text
Y2K / Wipeout-style telemetry interface
```

The login should feel like an entry gate into the benchmark telemetry system,
not a generic AWS auth form.

Primary visual language:

- off-white console background
- hard black panel borders
- clipped/chamfered panel corners
- orange, blue, and acid-lime accents
- dense telemetry labels
- high-contrast buttons
- mono labels for technical metadata
- condensed display type for headings

Recommended palette:

```css
:root {
  --dpm-bg: #f5f1e8;
  --dpm-ink: #111119;
  --dpm-muted: #34343f;
  --dpm-orange: #ff4d00;
  --dpm-blue: #004dff;
  --dpm-lime: #c6ff00;
  --dpm-panel: rgba(255, 255, 255, 0.82);
}
```

Recommended type system:

```css
--font-display: Impact, Haettenschweiler, "Arial Narrow Bold", "Arial Black", sans-serif;
--font-body: Arial, Helvetica, sans-serif;
--font-technical: "SFMono-Regular", Consolas, "Liberation Mono", Menlo, monospace;
```

Use display type for:

- page headline
- primary action labels if the UI permits
- section headings

Use technical mono for:

- helper labels
- "DPM//LOGIN//TELEMETRY" style tags
- footer/security metadata

Use body type for:

- input labels
- validation messages
- legal/security text

## Logo asset

Upload a simple black-on-transparent or black/orange SVG/PNG mark.

Suggested lockup text:

```text
DPM//BENCH
RED TEAM TELEMETRY
```

If Cognito only allows a single logo image, create a horizontal SVG/PNG with:

- black `DPM//BENCH` wordmark
- small orange slash or speed bar
- optional lime "AUTH GATE" capsule

Keep the logo compact. The form panel and CTA should remain the focal point.

## Page copy

Recommended headline:

```text
Enter the red-team benchmark grid.
```

Recommended subcopy:

```text
Sign in to replay deterministic memory scenarios, inspect evidence coverage,
and compare benchmark telemetry.
```

Recommended primary button:

```text
Launch session
```

Recommended secondary/security text:

```text
DPM benchmark access is authenticated through Cognito. Results, scenario
telemetry, and evaluator outputs remain tied to your run identity.
```

If Cognito does not allow headline/subcopy edits in Classic Hosted UI, include
the headline in the uploaded logo image or switch to Managed Login Branding.

## Classic Hosted UI CSS direction

Classic Hosted UI selectors can vary, so treat this as a starting point. Apply
in the Cognito branding CSS editor and preview carefully.

```css
body {
  background:
    linear-gradient(115deg, transparent 0 60%, rgba(0, 77, 255, 0.12) 60% 68%, transparent 68%),
    repeating-linear-gradient(90deg, rgba(17, 17, 25, 0.08) 0 1px, transparent 1px 42px),
    #f5f1e8 !important;
  color: #111119 !important;
  font-family: Arial, Helvetica, sans-serif !important;
}

.banner-customizable,
.background-customizable {
  background: transparent !important;
}

.modal-content,
.panel,
.cognito-asf,
.login-container {
  border: 3px solid #111119 !important;
  border-radius: 0 !important;
  box-shadow: 12px 12px 0 rgba(17, 17, 25, 0.95) !important;
  background: rgba(255, 255, 255, 0.82) !important;
  clip-path: polygon(0 0, calc(100% - 18px) 0, 100% 18px, 100% 100%, 18px 100%, 0 calc(100% - 18px));
}

h1,
h2,
.heading,
.label-customizable {
  color: #111119 !important;
  font-family: Impact, Haettenschweiler, "Arial Narrow Bold", "Arial Black", sans-serif !important;
  letter-spacing: -0.035em !important;
  text-transform: uppercase !important;
}

label,
.input-label,
.form-label {
  color: #111119 !important;
  font-family: "SFMono-Regular", Consolas, "Liberation Mono", Menlo, monospace !important;
  font-size: 0.78rem !important;
  font-weight: 800 !important;
  letter-spacing: 0.08em !important;
  text-transform: uppercase !important;
}

input {
  border: 2px solid #111119 !important;
  border-radius: 0 !important;
  background: #fff !important;
  color: #111119 !important;
  box-shadow: inset 4px 0 0 #004dff !important;
}

input:focus {
  border-color: #ff4d00 !important;
  box-shadow:
    inset 4px 0 0 #ff4d00,
    0 0 0 3px rgba(198, 255, 0, 0.75) !important;
  outline: none !important;
}

button,
.btn,
.submitButton-customizable {
  border: 3px solid #111119 !important;
  border-radius: 0 !important;
  background: #ff4d00 !important;
  color: #111119 !important;
  font-family: "SFMono-Regular", Consolas, "Liberation Mono", Menlo, monospace !important;
  font-weight: 950 !important;
  letter-spacing: 0.08em !important;
  text-transform: uppercase !important;
  box-shadow: 6px 6px 0 #111119 !important;
  clip-path: polygon(0 0, calc(100% - 12px) 0, 100% 12px, 100% 100%, 12px 100%, 0 calc(100% - 12px));
}

button:hover,
.btn:hover,
.submitButton-customizable:hover {
  background: #c6ff00 !important;
  transform: translate(-2px, -2px);
  box-shadow: 8px 8px 0 #111119 !important;
}

a {
  color: #004dff !important;
  font-weight: 800 !important;
}

.errorMessage-customizable,
.alert-danger,
.error {
  border-left: 8px solid #ff4d00 !important;
  background: #111119 !important;
  color: #f5f1e8 !important;
}
```

## Managed Login Branding direction

If Managed Login Branding is enabled, use the same system but prefer native
controls over brittle selector overrides:

- set page background to `#f5f1e8`
- set primary button to orange `#ff4d00`
- set focus/highlight color to lime `#c6ff00`
- set link/accent color to blue `#004dff`
- upload the DPM//BENCH logo
- use a clipped black-bordered card treatment where the editor allows custom
  HTML/CSS
- enable passkeys if available

Suggested managed-login card HTML, if custom HTML is supported:

```html
<div class="dpm-auth-frame">
  <div class="dpm-auth-tag">DPM//AUTH//BENCHMARK</div>
  <h1>Enter the red-team benchmark grid.</h1>
  <p>
    Replay deterministic memory scenarios, inspect evidence coverage, and
    compare benchmark telemetry.
  </p>
</div>
```

Suggested CSS for that custom block:

```css
.dpm-auth-frame {
  border: 3px solid #111119;
  background: rgba(255, 255, 255, 0.82);
  box-shadow: 12px 12px 0 rgba(17, 17, 25, 0.95);
  color: #111119;
  padding: 18px;
  clip-path: polygon(0 0, calc(100% - 18px) 0, 100% 18px, 100% 100%, 18px 100%, 0 calc(100% - 18px));
}

.dpm-auth-tag {
  color: #ff4d00;
  font-family: "SFMono-Regular", Consolas, "Liberation Mono", Menlo, monospace;
  font-size: 0.76rem;
  font-weight: 950;
  letter-spacing: 0.14em;
  text-transform: uppercase;
}

.dpm-auth-frame h1 {
  margin: 8px 0;
  font-family: Impact, Haettenschweiler, "Arial Narrow Bold", "Arial Black", sans-serif;
  font-size: clamp(2.4rem, 6vw, 4.6rem);
  line-height: 0.9;
  letter-spacing: -0.04em;
  text-transform: uppercase;
}
```

## Motion/state recommendations

Use motion sparingly because this is an auth surface:

- hover: buttons move `translate(-2px, -2px)` and increase hard shadow
- focus: lime outline or ring
- loading: short label like `AUTH GATE OPENING`
- error: black warning panel with orange left rail
- success/verification: lime rail with `IDENTITY LOCKED`

Avoid:

- flashing animation
- continuous high-speed movement
- anything that makes form fields harder to read

## Version control later

Assets uploaded through Cognito live in Cognito, not this repo.

When the design is finalized, export the managed-login branding config and
check it in:

```sh
aws cognito-idp describe-managed-login-branding \
  --region eu-north-1 \
  --user-pool-id eu-north-1_p5gQLyNUG \
  > tools/benchmarks/dpm_projection_cliff/cognito/branding.json
```

If that path does not exist yet, create it as part of the benchmark suite work.

## QA checklist

- Preview Classic Hosted UI and Managed Login v2 before publishing.
- Test desktop and mobile widths.
- Test email/password, forgot password, confirmation code, and passkey flows.
- Verify focus rings are visible.
- Verify error messages remain readable.
- Verify hosted page still redirects to:

```text
http://localhost:53682/callback
```

- Confirm the final URL remains under:

```text
dpm-bench-095713295645.auth.eu-north-1.amazoncognito.com
```
