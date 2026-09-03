import { component$ } from "@builder.io/qwik";
import type { DocumentHead } from "@builder.io/qwik-city";
import { routeLoader$ } from "@builder.io/qwik-city";
import { CctvHome } from "~/components/cctv/cctv-home";
import { loadCameras } from "~/lib/cameras";

export const useCameras = routeLoader$(async ({ env }) => {
  return loadCameras(env.get("PUBLIC_API_BASE_URL"));
});

export default component$(() => {
  const cameraData = useCameras();
  return (
    <CctvHome
      cameras={cameraData.value.cameras}
      source={cameraData.value.source}
      apiError={cameraData.value.error}
    />
  );
});

export const head: DocumentHead = {
  title: "Camera overview · Cachicamas CCTV",
  meta: [
    {
      name: "description",
      content: "Local network CCTV monitoring for ESP32-CAM devices.",
    },
  ],
};
