import { component$, isDev } from "@builder.io/qwik";
import { QwikCityProvider, RouterOutlet } from "@builder.io/qwik-city";
import { RouterHead } from "./components/router-head/router-head";

import "./global.css";

export default component$(() => {
  /**
   * The root of a QwikCity site always start with the <QwikCityProvider> component,
   * immediately followed by the document's <head> and <body>.
   *
   * Don't remove the `<head>` and `<body>` elements.
   */

  return (
    <QwikCityProvider>
      <head>
        <meta charset="utf-8" />
        {!isDev && (
          <link
            rel="manifest"
            href={`${import.meta.env.BASE_URL}manifest.json`}
          />
        )}
        <RouterHead />
      </head>
      <body lang="en" data-design-seed="096a14a1">
        {/* THESIS: This control-room homepage makes live camera state the primary surface, refusing a marketing hero or dashboard metric wall. OWN-WORLD: ink navy, warm amber, cool cyan, crisp rules, and field-console typography form a restrained night-shift instrument. STORY: the operator sees the local camera registry and each discovered camera's real relay state. FIRST VIEWPORT: a narrow utility rail keeps Cameras as the only destination while a live wall gives every discovered device a dedicated feed. FORM: grounded operate direction candidate 5, seed 096a14a1. FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md */}
        <RouterOutlet />
      </body>
    </QwikCityProvider>
  );
});
