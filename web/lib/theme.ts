import { createTheme, responsiveFontSizes } from "@mui/material/styles";

// A small, deliberately plain MUI theme -- docs/SERVER_PLAN.md §7.1: "MUI
// components and theme; no other UI kit". Nothing product-specific belongs
// here yet; kept in its own file so it is the one place to touch for a
// palette/typography change.
let theme = createTheme({
  palette: {
    mode: "light",
    primary: { main: "#2f5d8a" },
    secondary: { main: "#8a5d2f" },
  },
  shape: { borderRadius: 8 },
  components: {
    // Phones: a bare TextField fills its row (it wraps onto its own line in a
    // flex-wrap Stack) unless the call site sets an explicit width via `sx`;
    // inside a table cell it keeps its natural width.
    MuiTextField: {
      styleOverrides: {
        root: ({ theme }) => ({
          [theme.breakpoints.down("sm")]: {
            width: "100%",
            ".MuiTableCell-root &": { width: "auto" },
          },
        }),
      },
    },
    // Long device ids / IMSIs / hashes wrap instead of widening the page.
    MuiTableCell: {
      styleOverrides: {
        root: ({ theme }) => ({
          [theme.breakpoints.down("md")]: { overflowWrap: "anywhere" },
        }),
      },
    },
    MuiTypography: {
      styleOverrides: {
        h4: { overflowWrap: "break-word" },
        h5: { overflowWrap: "break-word" },
        h6: { overflowWrap: "break-word" },
      },
    },
  },
});
theme = responsiveFontSizes(theme);

export default theme;
