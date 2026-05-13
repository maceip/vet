import "./Decor.css";

/**
 * Art-deco decorative corners — image assets supplied by user
 * (ad1.png top-left, ad2.png top-right, ad3.png mid-right slim,
 * ad4.png bottom-right). Purely visual; no interaction.
 */
export default function Decor() {
  return (
    <div className="decor" aria-hidden="true">
      <img className="decor__img decor__topLeft" src="/vet/art/ad1.png" alt="" />
      <img className="decor__img decor__topRight" src="/vet/art/ad2.png" alt="" />
      <img className="decor__img decor__midRight" src="/vet/art/ad3.png" alt="" />
      <img className="decor__img decor__bottomRight" src="/vet/art/ad4.png" alt="" />
    </div>
  );
}
