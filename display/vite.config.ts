import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

const gatewayPort = Number(process.env.VIGIL_WS_PORT ?? 8081);

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    // In development the gateway runs separately; forward the WebSocket to it.
    proxy: {
      "/ws": { target: `ws://localhost:${gatewayPort}`, ws: true },
    },
  },
});
