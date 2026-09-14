import { createTheme } from "@mui/material/styles";

// A small, deliberately plain MUI theme -- docs/SERVER_PLAN.md §7.1: "MUI
// components and theme; no other UI kit". Nothing product-specific belongs
// here yet; kept in its own file so it is the one place to touch for a
// palette/typography change.
const theme = createTheme({
  palette: {
    mode: "light",
    primary: { main: "#2f5d8a" },
    secondary: { main: "#8a5d2f" },
  },
  shape: { borderRadius: 8 },
});

export default theme;
