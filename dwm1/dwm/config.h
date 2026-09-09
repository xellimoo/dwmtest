/* See LICENSE file for copyright and license details. */

/* appearance */
static const unsigned int borderpx  = 2;        /* border pixel of windows */
static const unsigned int snap      = 16;       /* snap pixel */
static const int showbar            = 1;        /* 0 means no bar */
static const int topbar             = 1;        /* 0 means bottom bar */
static const char *fonts[]          = { 
	"JetBrains Mono:size=10:antialias=true:autohint=true",
	"Symbols Nerd Font:size=10:antialias=true:autohint=true"
};
static const char dmenufont[]       = "JetBrains Mono:size=10:antialias=true:autohint=true";
static const char col_gray1[]       = "#222222";
static const char col_gray2[]       = "#444444";
static const char col_gray3[]       = "#bbbbbb";
static const char col_gray4[]       = "#eeeeee";
static const char col_cyan[]        = "#005577";
static const char col_urgent[]      = "#ff5555";
static const char col_dim[]         = "#777777";
static const char *colors[][3]      = {
	/*               fg         bg         border   */
	[SchemeNorm]     = { col_gray3, col_gray1, col_gray2 },
	[SchemeSel]      = { col_gray4, col_cyan,  col_cyan  },
	[SchemeTagSel]   = { col_gray4, col_cyan,  col_cyan  },
	[SchemeTaskSel]  = { col_gray4, col_cyan,  col_cyan  },
	[SchemeUrg]      = { col_gray1, col_urgent, col_urgent },
	[SchemeHid]      = { col_dim,   col_gray1, col_gray2 },
	[SchemeStatus]   = { col_gray3, col_gray1, col_gray2 },
};

/* runtime themes: INI-like files scanned at startup; MODKEY-n cycles them,
 * the built-in rows above are the always-present "default" theme */
static const char themedir[]        = "~/.config/edwm/themes";

/* tagging */
// static const char *tags[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };
static const char *tags[] = { "1", "2", "3" };

static const Rule rules[] = {
	/* xprop(1):
	 *	WM_CLASS(STRING) = instance, class
	 *	WM_NAME(STRING) = title
	 */
	/* class      instance    title       tags mask     isfloating   monitor */
    // don't need these any more, everything starts up floating
    { "Sample",                NULL,       NULL,      0,             1,           -1 },
    // { "Gimp",               NULL,       NULL,      1 << 4,        1,           -1 },
    // { "burp-StartBurp",     NULL,       NULL,      1 << 3,        1,           -1 },
    // { "st-256color",        NULL,       NULL,      0,             1,           -1 },
    // { "Wireshark",          NULL,       NULL,      0,             1,           -1 },
    // { "vlc",                NULL,       NULL,      0,             1,           -1 },
    // { "Xfe",                NULL,       NULL,      0,             1,           -1 },
    // { "XpdfReader",         NULL,       NULL,      0,             1,           -1 },
    // { "libreoffice-writer", NULL,       NULL,      0,             1,           -1 },
    // { "ksnip",              NULL,       NULL,      0,             1,           -1 },
    // { "Marker",             NULL,       NULL,      0,             1,           -1 },
    // { "XTerm",              NULL,       NULL,      0,             1,           -1 },
    // { "feh",                NULL,       NULL,      0,             1,           -1 },
};

/* layout(s) */
static const float mfact     = 0.55; /* factor of master area size [0.05..0.95] */
static const int nmaster     = 1;    /* number of clients in master area */
static const int resizehints = 1;    /* 1 means respect size hints in tiled resizals */
static const int lockfullscreen = 1; /* 1 will force focus on the fullscreen window */
static const int movespeed   = 16; /* px per keypress for keyboard window move */
static const int resizespeed = 16; /* px per keypress for keyboard window resize */
static const int chordwait = 150; /* ms to hold a plain press for chord detection */
static const int chorddist = 15;  /* px of movement that ends the hold (plain drag) */

static const Layout layouts[] = {
	/* symbol     arrange function */
    { "><>",      tile },    /* first entry is default */
    { "[T]",      NULL }    /* no layout function means floating behavior, meaning in task view */
	// { "[M]",      monocle },
};

/* key definitions */
#define MODKEY Mod1Mask
#define TAGKEYS(KEY,TAG) \
	{ MODKEY,                       KEY,      view,           {.ui = 1 << TAG} }, \
	{ MODKEY|ControlMask,           KEY,      toggleview,     {.ui = 1 << TAG} }, \
	{ MODKEY|ShiftMask,             KEY,      tag,            {.ui = 1 << TAG} }, \
	{ MODKEY|ControlMask|ShiftMask, KEY,      toggletag,      {.ui = 1 << TAG} },

/* helper for spawning shell commands in the pre dwm-5.0 fashion */
#define SHCMD(cmd) { .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }

/* commands */
static char dmenumon[2] = "0"; /* component of dmenucmd, manipulated in spawn() */
/* the theme_dmenu_* buffers are rewritten on every theme switch so dmenu
 * follows the WM colors; like dmenumon the pointers stay valid forever */
static const char *dmenucmd[] = { "dmenu_run", "-m", dmenumon, "-fn", dmenufont, "-nb", theme_dmenu_nb, "-nf", theme_dmenu_nf, "-sb", theme_dmenu_sb, "-sf", theme_dmenu_sf, NULL };
static const char *termcmd[]  = { "qterminal", NULL };
static const char *slockcmd[] = { "sh", "-c", "slock & sleep 0.2 && xset dpms force off", NULL };
static const char *ksnipcmd[]  = { "ksnip", "-r", NULL };
static const char *wordlistcmd[]  = { "python", "/home/ell/.scripts/wordlist-input.py", NULL };
static const char *webshellcmd[]  = { "python", "/home/ell/.scripts/webshell-input.py", NULL };
static const char *brightness_up[]  = { "backlight", "-f", "/dev/backlight/backlight0", "incr", "3", NULL };
static const char *brightness_down[]  = { "backlight", "-f", "/dev/backlight/backlight0", "decr", "3", NULL };
static const char *brightness_reset[]  = { "backlight", "-f", "/dev/backlight/backlight0", "19", NULL };

static const Key keys[] = {
	/* modifier                     key        function        argument */
	{ MODKEY,                       XK_r,      spawn,          {.v = dmenucmd } },
	{ MODKEY,             		    XK_c,      spawn,     	   {.v = ksnipcmd } },
	{ MODKEY,             		    XK_a,      spawnxterm,     {0} },
	{ MODKEY|ShiftMask,             XK_Return, spawn,          {.v = termcmd } },
	{ MODKEY|ShiftMask,             XK_p,      spawn,          {.v = slockcmd } },
	{ MODKEY|ShiftMask,             XK_u,      spawn,          {.v = wordlistcmd } },
	{ MODKEY|ShiftMask,             XK_i,      spawn,          {.v = webshellcmd } },
	{ MODKEY|ShiftMask,             XK_F7,     spawn,          {.v = brightness_up } },
	{ MODKEY|ShiftMask,             XK_F6,     spawn,          {.v = brightness_down } },
	{ MODKEY|ShiftMask,             XK_F8,     spawn,          {.v = brightness_reset } },
	{ MODKEY,                       XK_b,      togglebar,      {0} },
	{ MODKEY,                       XK_n,      cycletheme,     {.i = +1 } },
	{ MODKEY|ShiftMask,             XK_n,      cycletheme,     {.i = -1 } },
	{ MODKEY,                       XK_q,      raisetotop,     {0} },
	{ MODKEY,                       XK_v,      staylow,        {0} },
	{ MODKEY,                       XK_Return, zoom,           {0} },
	{ MODKEY,                       XK_t,      view,           {0} },
	{ MODKEY,                       XK_w,      focusstack,     {.i = -1 } },
	{ MODKEY|ShiftMask,             XK_w,      focusstack,     {.i = +1 } },
    { MODKEY,                       XK_Tab,    doswitchclient, {0} },
    { MODKEY,                       XK_e,      snapsidebyside, {0} },
    { MODKEY,                       XK_s,      swapsidebyside, {0} },
    { MODKEY,                       XK_j,      snap2left,      {0} },
    { MODKEY,                       XK_k,      snap2right,     {0} },
    { MODKEY|ShiftMask,             XK_j,      movey,          {.i = +15} }, /* pair-adjust when snapped, else move down */
    { MODKEY|ShiftMask,             XK_k,      movey,          {.i = -15} }, /* pair-adjust when snapped, else move up */
    { MODKEY|ShiftMask,             XK_h,      movex,          {.i = -1} },
    { MODKEY|ShiftMask,             XK_l,      movex,          {.i = +1} },
    { MODKEY|ShiftMask,             XK_q,      resizeh,        {.i = -1} },
    { MODKEY|ShiftMask,             XK_s,      resizeh,        {.i = +1} },
    { MODKEY|ShiftMask,             XK_e,      resizew,        {.i = -1} },
    { MODKEY|ShiftMask,             XK_d,      resizew,        {.i = +1} },
	{ MODKEY|ShiftMask,             XK_c,      killclient,     {0} },
	{ MODKEY,                       XK_d,      setlayout,      {.v = &layouts[0]} },
    { MODKEY,                       XK_f,      togglefullscreen,  {0} },
	{ MODKEY|ShiftMask,             XK_g,      togglefloating, {0} },
	TAGKEYS(                        XK_1,                      0)
	TAGKEYS(                        XK_2,                      1)
	TAGKEYS(                        XK_3,                      2)
	{ MODKEY|ShiftMask,             XK_bracketright,      quit,           {0} },
    // TAGKEYS(                        XK_4,                      3)
    // TAGKEYS(                        XK_q,                      4)
    // TAGKEYS(                        XK_w,                      5)
    // TAGKEYS(                        XK_e,                      6)
    // TAGKEYS(                        XK_r,                      7)
    // TAGKEYS(                        XK_z,                      8)
    // { MODKEY,                       XK_f,      setlayout,      {.v = &layouts[1]} },
	// { MODKEY,                       XK_m,      setlayout,      {.v = &layouts[2]} },
    // { MODKEY,                       XK_i,      incnmaster,     {.i = +1 } },
	// { MODKEY,                       XK_j,      focusstack,     {.i = +1 } },
	// { MODKEY,                       XK_k,      focusstack,     {.i = -1 } },
	// { MODKEY,                       XK_h,      setmfact,       {.f = -0.05} },
	// { MODKEY,                       XK_l,      setmfact,       {.f = +0.05} },
	// { MODKEY,                       XK_0,      view,           {.ui = ~0 } },
	// { MODKEY|ShiftMask,             XK_0,      tag,            {.ui = ~0 } },
	// { MODKEY,                       XK_comma,  focusmon,       {.i = -1 } },
	// { MODKEY,                       XK_period, focusmon,       {.i = +1 } },
	// { MODKEY|ShiftMask,             XK_comma,  tagmon,         {.i = -1 } },
	// { MODKEY|ShiftMask,             XK_period, tagmon,         {.i = +1 } },
	// { MODKEY,                       XK_space,  setlayout,      {0} },
};

/* button definitions */
/* click can be ClkTagBar, ClkLtSymbol, ClkStatusText, ClkWinTitle, ClkTaskButton, ClkClientWin, or ClkRootWin */
static const Button buttons[] = {
	/* click                event mask      button          function        argument */
	{ ClkLtSymbol,          0,              Button1,        setlayout,      {0} },
	{ ClkLtSymbol,          0,              Button3,        setlayout,      {0} },
	{ ClkWinTitle,          0,              Button2,        zoom,           {0} },
	{ ClkTaskButton,        0,              Button1,        taskactivate,   {0} },
	{ ClkTaskButton,        0,              Button2,        taskzoom,       {0} },
	{ ClkTaskButton,        0,              Button3,        taskminimize,   {0} },
	{ ClkStatusText,        0,              Button2,        spawn,          {.v = termcmd } },
	//{ ClkClientWin,         0,              Button2,        movemouse,      {0} },
	//{ ClkClientWin,         MODKEY,         Button2,        togglefloating, {0} },
	{ ClkClientWin,         MODKEY,         Button2,        movemouse,      {0} },
	//{ ClkClientWin,         MODKEY,         Button3,        resizemouse,    {0} },
    { ClkClientWin,         0,              Button2,        resizemouse,    {0} },
	{ ClkTagBar,            0,              Button1,        view,           {0} },
	{ ClkTagBar,            0,              Button3,        toggleview,     {0} },
	{ ClkTagBar,            MODKEY,         Button1,        tag,            {0} },
	{ ClkTagBar,            MODKEY,         Button3,        toggletag,      {0} },
};

