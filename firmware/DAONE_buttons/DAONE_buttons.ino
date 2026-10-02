#include "secrets.h"  // Copy secrets.example.h to secrets.h and fill in your credentials.
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
// ==========================================
// TFT
// ==========================================

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite lyricSprite = TFT_eSprite(&tft);

// ==========================================
// WIFI
// ==========================================



// ==========================================
// SPOTIFY
// ==========================================




String accessToken = "";
String lastSong = "";
String lastArtist = "";
bool lastPlaying = false;


// ==========================================
// BUMPER BUTTONS
// ==========================================

const int LEFT_BUTTON_PIN = 6;
const int RIGHT_BUTTON_PIN = 7;

const unsigned long BUTTON_DEBOUNCE_MS = 35;
const unsigned long DOUBLE_CLICK_MS = 300;

struct BumperButton {
  int pin;
  bool stableState;
  bool lastRawState;
  unsigned long lastRawChange;
  uint8_t clickCount;
  unsigned long firstClickAt;
};

BumperButton leftBumper = {
  LEFT_BUTTON_PIN, HIGH, HIGH, 0, 0, 0
};

BumperButton rightBumper = {
  RIGHT_BUTTON_PIN, HIGH, HIGH, 0, 0, 0
};


// ============================================================
// LYRIC DATA
// ============================================================

String syncedLyrics = "";

String currentLyricLine = "";
String previousLyricLine = "";
String nextLyricLine = "";

unsigned long currentLyricStart = 0;
unsigned long nextLyricStart = 0;


// ============================================================
// SPOTIFY TIMING
// ============================================================

unsigned long spotifyProgressMs = 0;
unsigned long progressReceivedAt = 0;
unsigned long spotifyDurationMs = 0;
bool lyricClockResetRequested = false;

// ============================================================
// UI COLORS
// ============================================================

const uint16_t UI_BACKGROUND = TFT_BLACK;

// Very dark blue/gray card
const uint16_t UI_CARD = 0x0841;

// Subtle border
const uint16_t UI_BORDER = 0x2945;

// Muted gray text
const uint16_t UI_MUTED = 0x7BEF;

const uint16_t UI_GREEN = TFT_GREEN;


// ============================================================
// DRAWING STATE
// ============================================================

int lastHighlightPixels = -1;


// ============================================================
// CURRENT LYRIC LAYOUT
// ============================================================

const int MAX_LYRIC_LINES = 4;

String lyricDisplayLines[MAX_LYRIC_LINES];

int lyricLineCount = 0;

int lyricTextSize = 2;
int lyricCharWidth = 12;
int lyricCharHeight = 16;

int lyricX[MAX_LYRIC_LINES];
int lyricY[MAX_LYRIC_LINES];

int lyricTotalCharacters = 0;

// ==========================================
// WORD-LEVEL KARAOKE TIMING
// ==========================================

struct KaraokeWord {
  unsigned long startMs;
  String text;

  // Position of this word in the clean lyric line
  int charStart;
  int charEnd;
};

const int MAX_KARAOKE_WORDS = 500;

KaraokeWord karaokeWords[MAX_KARAOKE_WORDS];

int karaokeWordCount = 0;

bool wordTimedLyricsAvailable = false;
bool lineTimedLyricsAvailable = false;

// ============================================================
// BACKGROUND SPOTIFY DATA
// ============================================================

SemaphoreHandle_t spotifyMutex = NULL;

String pendingAlbumArtUrl = "";
String pendingSong = "";
String pendingArtist = "";
String albumArtUrl = "";

bool pendingPlaying = false;

unsigned long pendingProgressMs = 0;
unsigned long pendingDurationMs = 0;
unsigned long pendingReceivedAt = 0;

int pendingSpotifyStatus = 0;

bool spotifyDataReady = false;

// ============================================================
// NETWORK REQUEST COORDINATION
// ============================================================

// The Spotify FreeRTOS task normally polls in the background.
// These flags temporarily pause that polling while the main loop
// is downloading lyrics so two TLS/HTTP requests do not overlap.
volatile bool pauseSpotifyPolling = false;
volatile bool spotifyRequestActive = false;


// ============================================================
// FUNCTION DECLARATIONS
// ============================================================

String getSpotifyAccessToken();

String getLyrics(
  String song,
  String artist
);

void updateLyricsDisplay();

void drawPlaybackScreen(
  String song,
  String artist,
  bool playing
);

void displayNothingPlaying();

void drawLyricScreen();

void drawKaraokeHighlight(
  float progress
);

void drawCenteredText(
  String text,
  int centerY,
  int textSize,
  uint16_t color
);

void prepareLyricLayout();

void drawBottomProgress();

void spotifyTask(
  void *parameter
);

void fetchSpotifyInBackground();

void processSpotifyUpdate();


void updateBumperButtons();
void updateOneBumper(BumperButton &button, bool isLeft);
void handleSingleBumperPress();
void handleDoubleBumperPress(bool isLeft);
bool sendSpotifyControl(const String &url, const String &method);
void spotifyPlayPause();
void spotifyReplay();
void spotifyNext();



// ============================================================
// BUMPER BUTTONS + SPOTIFY PLAYBACK CONTROLS
// ============================================================

bool sendSpotifyControl(const String &url, const String &method) {

  if (accessToken.length() == 0 || WiFi.status() != WL_CONNECTED) {
    Serial0.println("Playback control unavailable: no Spotify connection.");
    return false;
  }

  // Do not overlap a control request with the background Spotify HTTPS request.
  pauseSpotifyPolling = true;

  unsigned long waitStarted = millis();
  while (spotifyRequestActive && millis() - waitStarted < 3000) {
    delay(10);
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  if (!https.begin(client, url)) {
    Serial0.println("Could not start Spotify control request.");
    pauseSpotifyPolling = false;
    return false;
  }

  https.addHeader("Authorization", "Bearer " + accessToken);
  https.addHeader("Content-Length", "0");

  int httpCode = -1;

  if (method == "POST") {
    httpCode = https.POST("");
  } else {
    httpCode = https.PUT("");
  }

  https.end();
  pauseSpotifyPolling = false;

  Serial0.print("Spotify control HTTP code: ");
  Serial0.println(httpCode);

  // Spotify playback-control endpoints normally return 204 on success.
  return httpCode >= 200 && httpCode < 300;
}


void spotifyPlayPause() {

  if (lastPlaying) {
    Serial0.println("BUMPER: pause");

    if (sendSpotifyControl(
      "https://api.spotify.com/v1/me/player/pause",
      "PUT"
    )) {
      lastPlaying = false;
    }
  }

  else {
    Serial0.println("BUMPER: play");

    if (sendSpotifyControl(
      "https://api.spotify.com/v1/me/player/play",
      "PUT"
    )) {
      lastPlaying = true;
    }
  }
}


void spotifyReplay() {

  Serial0.println("BUMPER: replay");

  if (sendSpotifyControl(
    "https://api.spotify.com/v1/me/player/seek?position_ms=0",
    "PUT"
  )) {
    spotifyProgressMs = 0;
    progressReceivedAt = millis();
    lyricClockResetRequested = true;
  }
}


void spotifyNext() {

  Serial0.println("BUMPER: skip");

  sendSpotifyControl(
    "https://api.spotify.com/v1/me/player/next",
    "POST"
  );
}


void handleSingleBumperPress() {
  // A single press on EITHER bumper toggles play/pause.
  spotifyPlayPause();
}


void handleDoubleBumperPress(bool isLeft) {

  if (isLeft) {
    spotifyReplay();
  }

  else {
    spotifyNext();
  }
}


void updateOneBumper(BumperButton &button, bool isLeft) {

  bool rawState = digitalRead(button.pin);
  unsigned long now = millis();

  if (rawState != button.lastRawState) {
    button.lastRawState = rawState;
    button.lastRawChange = now;
  }

  // Accept a state change only after it has remained stable long enough.
  if (
    rawState != button.stableState &&
    now - button.lastRawChange >= BUTTON_DEBOUNCE_MS
  ) {

    button.stableState = rawState;

    // Count clicks on RELEASE. This avoids long holds being counted twice.
    if (button.stableState == HIGH) {

      if (
        button.clickCount == 0 ||
        now - button.firstClickAt > DOUBLE_CLICK_MS
      ) {
        button.clickCount = 1;
        button.firstClickAt = now;
      }

      else {
        // Second click arrived inside the double-click window.
        button.clickCount = 0;
        handleDoubleBumperPress(isLeft);
      }
    }
  }

  // One click with no second click becomes a single press after the window expires.
  if (
    button.clickCount == 1 &&
    now - button.firstClickAt > DOUBLE_CLICK_MS
  ) {
    button.clickCount = 0;
    handleSingleBumperPress();
  }
}


void updateBumperButtons() {
  updateOneBumper(leftBumper, true);
  updateOneBumper(rightBumper, false);
}


// ============================================================
// GET SPOTIFY ACCESS TOKEN
// ============================================================

String getSpotifyAccessToken() {

  Serial0.println(
    "Getting Spotify access token..."
  );


  WiFiClientSecure client;

  client.setInsecure();


  HTTPClient https;


  if (!https.begin(
    client,
    "https://accounts.spotify.com/api/token"
  )) {

    Serial0.println(
      "Could not connect to Spotify."
    );

    return "";
  }


  https.addHeader(
    "Content-Type",
    "application/x-www-form-urlencoded"
  );


  String body =
    "grant_type=refresh_token"
    "&refresh_token=" +
    String(SPOTIFY_REFRESH_TOKEN) +
    "&client_id=" +
    String(SPOTIFY_CLIENT_ID) +
    "&client_secret=" +
    String(SPOTIFY_CLIENT_SECRET);


  int httpCode =
    https.POST(body);


  Serial0.print(
    "Token HTTP code: "
  );

  Serial0.println(
    httpCode
  );


  String response =
    https.getString();


  if (
    httpCode != 200
  ) {

    Serial0.println(
      "Spotify authentication failed."
    );

    https.end();

    return "";
  }


  JsonDocument doc;


  if (
    deserializeJson(
      doc,
      response
    )
  ) {

    Serial0.println(
      "Token JSON error."
    );

    https.end();

    return "";
  }


  String token =
    doc["access_token"]
    .as<String>();


  https.end();


  Serial0.println(
    "Spotify authenticated!"
  );


  return token;
}


// ============================================================
// CENTER TEXT HELPER
// ============================================================

void drawCenteredText(
  String text,
  int centerY,
  int textSize,
  uint16_t color
) {

  tft.setTextSize(
    textSize
  );


  tft.setTextColor(
    color,
    UI_BACKGROUND
  );


  int charWidth =
    6 * textSize;


  int textWidth =
    text.length() *
    charWidth;


  int x =
    (480 - textWidth) / 2;


  if (
    x < 10
  ) {

    x = 10;
  }


  tft.setCursor(
    x,
    centerY
  );


  tft.print(
    text
  );
}


// ============================================================
// PLAYBACK SCREEN
// ============================================================

void drawPlaybackScreen(
  String song,
  String artist,
  bool playing
) {

  tft.fillScreen(
    UI_BACKGROUND
  );


  // ==========================================================
  // BRAND
  // ==========================================================

  tft.setTextSize(
    1
  );


  tft.setTextColor(
    UI_GREEN,
    UI_BACKGROUND
  );


  tft.setCursor(
    14,
    9
  );


  tft.print(
    "LYRIC BUDDY"
  );


  // ==========================================================
  // PLAYING / PAUSED INDICATOR
  // ==========================================================

  if (
    playing
  ) {

    tft.fillTriangle(
      385,
      8,

      385,
      20,

      395,
      14,

      UI_GREEN
    );


    tft.setTextColor(
      UI_GREEN,
      UI_BACKGROUND
    );


    tft.setCursor(
      402,
      11
    );


    tft.print(
      "PLAYING"
    );

  }

  else {

    tft.fillRect(
      386,
      9,
      3,
      11,
      TFT_YELLOW
    );


    tft.fillRect(
      392,
      9,
      3,
      11,
      TFT_YELLOW
    );


    tft.setTextColor(
      TFT_YELLOW,
      UI_BACKGROUND
    );


    tft.setCursor(
      402,
      11
    );


    tft.print(
      "PAUSED"
    );
  }


  // ==========================================================
  // SONG TITLE
  // ==========================================================

  String displaySong =
    song;


  if (
    displaySong.length() > 38
  ) {

    displaySong =
      displaySong.substring(
        0,
        35
      );


    displaySong += "...";
  }


  tft.setTextSize(
    2
  );


  tft.setTextColor(
    TFT_WHITE,
    UI_BACKGROUND
  );


  tft.setCursor(
    14,
    31
  );


  tft.print(
    displaySong
  );


  // ==========================================================
  // ARTIST
  // ==========================================================

  String displayArtist =
    artist;


  if (
    displayArtist.length() > 55
  ) {

    displayArtist =
      displayArtist.substring(
        0,
        52
      );


    displayArtist += "...";
  }


  tft.setTextSize(
    1
  );


  tft.setTextColor(
    UI_MUTED,
    UI_BACKGROUND
  );


  tft.setCursor(
    15,
    54
  );


  tft.print(
    displayArtist
  );


  // ==========================================================
  // LYRIC CARD
  // ==========================================================

  tft.fillRoundRect(
    8,
    72,
    464,
    190,
    12,
    UI_CARD
  );


  tft.drawRoundRect(
    8,
    72,
    464,
    190,
    12,
    UI_BORDER
  );


  // Small green accent

  tft.fillRoundRect(
    205,
    79,
    70,
    3,
    2,
    UI_GREEN
  );


  drawBottomProgress();


  // Force lyric redraw

  currentLyricLine = "";
  previousLyricLine = "";
  nextLyricLine = "";

  lastHighlightPixels = -1;

  lyricLineCount = 0;
  lyricTotalCharacters = 0;
}


// ============================================================
// NOTHING PLAYING
// ============================================================

void displayNothingPlaying() {

  tft.fillScreen(
    UI_BACKGROUND
  );


  tft.setTextColor(
    UI_GREEN,
    UI_BACKGROUND
  );


  tft.setTextSize(
    3
  );


  tft.setCursor(
    120,
    85
  );


  tft.println(
    "LYRIC BUDDY"
  );


  tft.setTextColor(
    UI_MUTED,
    UI_BACKGROUND
  );


  tft.setTextSize(
    2
  );


  tft.setCursor(
    135,
    150
  );


  tft.println(
    "Nothing playing"
  );
}


// ============================================================
// BACKGROUND SPOTIFY REQUEST
// ============================================================

void fetchSpotifyInBackground() {

  if (
    accessToken.length() == 0
  ) {

    return;
  }


  WiFiClientSecure client;

  client.setInsecure();


  HTTPClient https;


  if (!https.begin(
    client,
    "https://api.spotify.com/v1/me/player/currently-playing"
  )) {

    Serial0.println(
      "Spotify background connection failed."
    );

    return;
  }


  https.addHeader(
    "Authorization",
    "Bearer " + accessToken
  );


  int httpCode =
    https.GET();


  Serial0.print(
    "Currently playing HTTP code: "
  );


  Serial0.println(
    httpCode
  );


  // ==========================================================
  // SONG FOUND
  // ==========================================================

  if (
    httpCode == 200
  ) {

    String response =
      https.getString();


    JsonDocument doc;


    if (
      deserializeJson(
        doc,
        response
      )
    ) {

      Serial0.println(
        "Song JSON parsing failed."
      );


      https.end();

      return;
    }


    String song =
      doc["item"]["name"]
      .as<String>();


    String artist =
      doc["item"]["artists"][0]["name"]
      .as<String>();


    bool playing =
      doc["is_playing"] |
      false;


    unsigned long progress =
      doc["progress_ms"] |
      0;


    unsigned long duration =
      doc["item"]["duration_ms"] |
      0;


    // ========================================================
    // GET ALBUM ART URL
    // ========================================================

    String albumUrl = "";


    JsonArray images =
      doc["item"]["album"]["images"]
      .as<JsonArray>();


    if (
      images.size() > 0
    ) {

      // Spotify normally gives multiple image sizes.
      // Use the smallest available image to reduce
      // download/memory requirements on the ESP32.

      int imageIndex =
        images.size() - 1;


      albumUrl =
        images[imageIndex]["url"]
        .as<String>();
    }


    unsigned long receivedAt =
      millis();


    // ========================================================
    // SEND DATA TO MAIN LOOP
    // ========================================================

    if (
      xSemaphoreTake(
        spotifyMutex,
        portMAX_DELAY
      ) == pdTRUE
    ) {

      pendingSong =
        song;


      pendingArtist =
        artist;


      pendingPlaying =
        playing;


      pendingProgressMs =
        progress;


      pendingDurationMs =
        duration;


      pendingReceivedAt =
        receivedAt;


      pendingAlbumArtUrl =
        albumUrl;


      pendingSpotifyStatus =
        200;


      spotifyDataReady =
        true;


      xSemaphoreGive(
        spotifyMutex
      );
    }
  }


  // ==========================================================
  // NOTHING PLAYING
  // ==========================================================

  else if (
    httpCode == 204
  ) {

    if (
      xSemaphoreTake(
        spotifyMutex,
        portMAX_DELAY
      ) == pdTRUE
    ) {

      pendingSpotifyStatus =
        204;


      spotifyDataReady =
        true;


      xSemaphoreGive(
        spotifyMutex
      );
    }
  }


  // ==========================================================
  // TOKEN EXPIRED
  // ==========================================================

  else if (
    httpCode == 401
  ) {

    Serial0.println(
      "Spotify token expired."
    );


    https.end();


    String newToken =
      getSpotifyAccessToken();


    if (
      newToken.length() > 0
    ) {

      accessToken =
        newToken;
    }


    return;
  }


  else {

    Serial0.println(
      "Spotify background request failed."
    );
  }


  https.end();
}


// ============================================================
// SPOTIFY FREERTOS TASK
// ============================================================

void spotifyTask(
  void *parameter
) {

  // Small startup delay
  vTaskDelay(
    pdMS_TO_TICKS(
      500
    )
  );


  while (
    true
  ) {

    // Only start a Spotify HTTPS request when the main loop is
    // not using the network for a lyrics download.
    if (!pauseSpotifyPolling) {

      spotifyRequestActive = true;

      fetchSpotifyInBackground();

      spotifyRequestActive = false;
    }


    // ========================================================
    // CHOOSE NEXT POLL SPEED
    // ========================================================

    // Normal polling:
    // Check Spotify once every 1 second.
    //
    // This means a manual skip should usually
    // be detected within about 0–1 second.

    unsigned long pollDelay = 1000;


    // ========================================================
    // CHECK IF WE ARE NEAR THE END OF THE SONG
    // ========================================================

    if (
      spotifyDurationMs > 0 &&
      lastPlaying
    ) {

      // Estimate the current playback position
      // using the last Spotify position plus
      // the time that has passed since we received it.

      unsigned long estimatedPosition =
        spotifyProgressMs;


      estimatedPosition +=
        millis() - progressReceivedAt;


      // ======================================================
      // LAST 5 SECONDS
      // ======================================================

      if (
        estimatedPosition < spotifyDurationMs
      ) {

        unsigned long remaining =
          spotifyDurationMs -
          estimatedPosition;


        if (
          remaining <= 5000
        ) {

          // Near the end of the song:
          // poll Spotify every 400 ms.

          pollDelay = 400;
        }
      }

      else {

        // Our local clock says the song should
        // already be finished.
        //
        // Keep checking quickly until Spotify
        // reports the next track.

        pollDelay = 400;
      }
    }


    // ========================================================
    // WAIT UNTIL NEXT SPOTIFY CHECK
    // ========================================================

    vTaskDelay(
      pdMS_TO_TICKS(
        pollDelay
      )
    );
  }
}

// ============================================================
// PROCESS SPOTIFY UPDATE
// ============================================================

void processSpotifyUpdate() {

  if (
    !spotifyDataReady
  ) {

    return;
  }


  String song = "";
  String artist = "";
  String newAlbumArtUrl = "";

  bool playing = false;

  unsigned long progress = 0;
  unsigned long duration = 0;
  unsigned long receivedAt = 0;

  int status = 0;


  if (
    xSemaphoreTake(
      spotifyMutex,
      0
    ) != pdTRUE
  ) {

    return;
  }


  if (
    !spotifyDataReady
  ) {

    xSemaphoreGive(
      spotifyMutex
    );

    return;
  }


  song =
    pendingSong;


  artist =
    pendingArtist;


  newAlbumArtUrl =
    pendingAlbumArtUrl;


  playing =
    pendingPlaying;


  progress =
    pendingProgressMs;


  duration =
    pendingDurationMs;


  receivedAt =
    pendingReceivedAt;


  status =
    pendingSpotifyStatus;


  spotifyDataReady =
    false;


  xSemaphoreGive(
    spotifyMutex
  );


  // ==========================================================
  // SONG CURRENTLY PLAYING
  // ==========================================================

  if (
    status == 200
  ) {

    Serial0.println();


    Serial0.print(
      "Song: "
    );


    Serial0.println(
      song
    );


    Serial0.print(
      "Artist: "
    );


    Serial0.println(
      artist
    );


    Serial0.print(
      "Progress: "
    );


    Serial0.print(
      progress
    );


    Serial0.println(
      " ms"
    );


    bool songChanged =
      song != lastSong ||
      artist != lastArtist;


    bool stateChanged =
      playing != lastPlaying;


    spotifyProgressMs =
      progress;


    spotifyDurationMs =
      duration;


    progressReceivedAt =
      receivedAt;


    // ========================================================
    // NEW SONG
    // ========================================================

    if (
      songChanged
    ) {

      // Save the album artwork URL for this song.

      albumArtUrl =
        newAlbumArtUrl;


      Serial0.print(
        "Album art: "
      );


      Serial0.println(
        albumArtUrl
      );

      //drawAlbumArt(
     //   albumArtUrl
      //);


      lyricClockResetRequested = true;

      syncedLyrics = "";

      currentLyricLine = "";
      previousLyricLine = "";
      nextLyricLine = "";

      currentLyricStart = 0;
      nextLyricStart = 0;

      lastHighlightPixels = -1;

      lyricLineCount = 0;
      lyricTotalCharacters = 0;


      drawPlaybackScreen(
        song,
        artist,
        playing
      );

      //drawAlbumArt(
     //   albumArtUrl
      //);


      // ======================================================
      // PAUSE SPOTIFY NETWORK POLLING WHILE FETCHING LYRICS
      // ======================================================

      pauseSpotifyPolling = true;

      // If the Spotify task was already in the middle of an HTTPS
      // request, let that request finish before starting SyncLRC
      // or LRCLIB. This prevents overlapping TLS connections from
      // interfering with one another.
      while (spotifyRequestActive) {
        delay(10);
      }

      syncedLyrics =
        getLyrics(
          song,
          artist
        );

      pauseSpotifyPolling = false;


      if (
        syncedLyrics.length() > 0
      ) {

        Serial0.println(
          "Synced lyrics successfully downloaded!"
        );
      }

      else {

        Serial0.println(
          "Could not find synced lyrics for this song."
        );


        // Put message inside lyric card.

        tft.setTextColor(
          UI_MUTED,
          UI_CARD
        );


        tft.setTextSize(
          2
        );


        tft.setCursor(
          130,
          160
        );


        tft.print(
          "Lyrics unavailable"
        );
      }
    }


    // ========================================================
    // PLAY / PAUSE CHANGED
    // ========================================================

    else if (
      stateChanged
    ) {

      // Clear old status area only.

      tft.fillRect(
        378,
        5,
        98,
        22,
        UI_BACKGROUND
      );


      if (
        playing
      ) {

        tft.fillTriangle(
          385,
          8,

          385,
          20,

          395,
          14,

          UI_GREEN
        );


        tft.setTextColor(
          UI_GREEN,
          UI_BACKGROUND
        );


        tft.setTextSize(
          1
        );


        tft.setCursor(
          402,
          11
        );


        tft.print(
          "PLAYING"
        );
      }

      else {

        tft.fillRect(
          386,
          9,
          3,
          11,
          TFT_YELLOW
        );


        tft.fillRect(
          392,
          9,
          3,
          11,
          TFT_YELLOW
        );


        tft.setTextColor(
          TFT_YELLOW,
          UI_BACKGROUND
        );


        tft.setTextSize(
          1
        );


        tft.setCursor(
          402,
          11
        );


        tft.print(
          "PAUSED"
        );
      }
    }


    lastSong =
      song;


    lastArtist =
      artist;


    lastPlaying =
      playing;
  }


  // ==========================================================
  // NOTHING PLAYING
  // ==========================================================

  else if (
    status == 204
  ) {

    if (
      lastSong != "" ||
      lastPlaying
    ) {

      displayNothingPlaying();


      lastSong = "";
      lastArtist = "";

      lastPlaying = false;

      syncedLyrics = "";

      currentLyricLine = "";
      previousLyricLine = "";
      nextLyricLine = "";

      lyricLineCount = 0;
      lyricTotalCharacters = 0;

      lastHighlightPixels = -1;

      spotifyDurationMs = 0;

      albumArtUrl = "";
    }
  }
}


// ============================================================
// GET LYRICS FROM LRCLIB
// ============================================================

// ==========================================
// GET WORD-SYNCED LYRICS FROM SYNCLRC
// ==========================================

// ==========================================
// GET WORD-SYNCED LYRICS FROM SYNCLRC
// ==========================================

String getLyrics(
  String song,
  String artist
) {

  Serial0.println();
  Serial0.println("Searching for lyrics...");

  Serial0.print("Song: ");
  Serial0.println(song);

  Serial0.print("Artist: ");
  Serial0.println(artist);


  // ==========================================================
  // RESET LYRIC MODE
  // ==========================================================

  karaokeWordCount = 0;
  wordTimedLyricsAvailable = false;
  lineTimedLyricsAvailable = false;


  // SyncLRC and LRCLIB each get their own WiFiClientSecure.
  // They are different HTTPS hosts, so reusing one TLS client
  // between them is unnecessary and can leave stale connection
  // state behind.


  // ==========================================================
  // 1. TRY SYNCLRC KARAOKE / WORD-TIMED LYRICS
  // ==========================================================

  {
    WiFiClientSecure syncClient;
    syncClient.setInsecure();

    HTTPClient https;

    String url =
      "https://api.synclrc.dev/lyrics?track=" +
      song +
      "&artist=" +
      artist +
      "&type=karaoke";

    url.replace(" ", "%20");


    Serial0.println(
      "Trying SyncLRC karaoke..."
    );


    if (https.begin(syncClient, url)) {

      https.addHeader(
        "User-Agent",
        "LyricBuddy/1.0"
      );


      int httpCode =
        https.GET();


      Serial0.print(
        "SyncLRC karaoke HTTP code: "
      );

      Serial0.println(httpCode);


      if (httpCode == 200) {

        JsonDocument doc;


// Parse the JSON directly from the HTTPS stream.
// This avoids storing the entire LRCLIB response
// in a temporary String first.

        WiFiClient *stream =
          https.getStreamPtr();


        DeserializationError error =
          deserializeJson(
            doc,
            *stream
          );


https.end();


        if (!error) {

          String enhancedLyrics =
            doc["lyrics"] | "";


          if (
            enhancedLyrics.length() > 0
          ) {

            String cleanLyrics = "";

            int position = 0;


            // ==================================================
            // PARSE KARAOKE LINES
            // ==================================================

            while (
              position <
              enhancedLyrics.length()
            ) {

              int lineStart =
                enhancedLyrics.indexOf(
                  '[',
                  position
                );


              if (lineStart == -1) {
                break;
              }


              int lineTimestampEnd =
                enhancedLyrics.indexOf(
                  ']',
                  lineStart
                );


              if (
                lineTimestampEnd == -1
              ) {
                break;
              }


              int lineEnd =
                enhancedLyrics.indexOf(
                  '\n',
                  lineTimestampEnd
                );


              if (lineEnd == -1) {

                lineEnd =
                  enhancedLyrics.length();
              }


              String lineTimestamp =
                enhancedLyrics.substring(
                  lineStart,
                  lineTimestampEnd + 1
                );


              String enhancedLine =
                enhancedLyrics.substring(
                  lineTimestampEnd + 1,
                  lineEnd
                );


              String cleanLine = "";

              int wordPosition = 0;


              // ==================================================
              // PARSE WORD TIMESTAMPS
              // ==================================================

              while (
                wordPosition <
                enhancedLine.length()
              ) {

                int timestampStart =
                  enhancedLine.indexOf(
                    '<',
                    wordPosition
                  );


                if (
                  timestampStart == -1
                ) {
                  break;
                }


                int timestampEnd =
                  enhancedLine.indexOf(
                    '>',
                    timestampStart
                  );


                if (
                  timestampEnd == -1
                ) {
                  break;
                }


                String timestamp =
                  enhancedLine.substring(
                    timestampStart + 1,
                    timestampEnd
                  );


                int nextTimestamp =
                  enhancedLine.indexOf(
                    '<',
                    timestampEnd + 1
                  );


                String wordWithSpacing;


                if (
                  nextTimestamp == -1
                ) {

                  wordWithSpacing =
                    enhancedLine.substring(
                      timestampEnd + 1
                    );

                } else {

                  wordWithSpacing =
                    enhancedLine.substring(
                      timestampEnd + 1,
                      nextTimestamp
                    );
                }


                String word =
                  wordWithSpacing;

                word.trim();


                int colon =
                  timestamp.indexOf(':');


                if (
                  colon != -1 &&
                  word.length() > 0 &&
                  karaokeWordCount <
                    MAX_KARAOKE_WORDS
                ) {

                  int minutes =
                    timestamp.substring(
                      0,
                      colon
                    ).toInt();


                  float seconds =
                    timestamp.substring(
                      colon + 1
                    ).toFloat();


                  unsigned long wordTimeMs =
                    (minutes * 60000UL) +
                    (unsigned long)(
                      seconds * 1000.0
                    );


                  int charStart =
                    cleanLine.length();


                  cleanLine += word;


                  int charEnd =
                    cleanLine.length();


                  karaokeWords[
                    karaokeWordCount
                  ].startMs =
                    wordTimeMs;


                  karaokeWords[
                    karaokeWordCount
                  ].text =
                    word;


                  karaokeWords[
                    karaokeWordCount
                  ].charStart =
                    charStart;


                  karaokeWords[
                    karaokeWordCount
                  ].charEnd =
                    charEnd;


                  karaokeWordCount++;


                  if (
                    nextTimestamp != -1
                  ) {
                    cleanLine += " ";
                  }
                }


                if (
                  nextTimestamp == -1
                ) {
                  break;
                }


                wordPosition =
                  nextTimestamp;
              }


              // Build normal line-level LRC for
              // the existing lyric display system.

              cleanLyrics +=
                lineTimestamp +
                cleanLine;


              if (
                lineEnd <
                enhancedLyrics.length()
              ) {

                cleanLyrics += "\n";
              }


              position =
                lineEnd + 1;
            }


            // ==================================================
            // KARAOKE SUCCESS
            // ==================================================

            if (
              karaokeWordCount > 0 &&
              cleanLyrics.length() > 0
            ) {

              wordTimedLyricsAvailable =
                true;

              lineTimedLyricsAvailable =
                false;


              Serial0.print(
                "KARAOKE MODE READY! Words: "
              );

              Serial0.println(
                karaokeWordCount
              );


              return cleanLyrics;
            }
          }

        } else {

          Serial0.print(
            "SyncLRC JSON error: "
          );

          Serial0.println(
            error.c_str()
          );
        }

      } else {

        https.end();
      }
    }
  }


  // ==========================================================
  // 2. KARAOKE FAILED
  //    TRY LRCLIB EXACT MATCH
  // ==========================================================

  Serial0.println(
    "Karaoke unavailable."
  );

  Serial0.println(
    "Trying LRCLIB exact match..."
  );

  karaokeWordCount = 0;
  wordTimedLyricsAvailable = false;

  // ==========================================================
  // LRCLIB EXACT MATCH
  // ==========================================================

  {
    WiFiClientSecure lrcClient;
    lrcClient.setInsecure();

    HTTPClient https;

    String url =
      "https://lrclib.net/api/get?track_name=" +
      song +
      "&artist_name=" +
      artist;

    url.replace(" ", "%20");

    if (
      https.begin(
        lrcClient,
        url
      )
    ) {

      https.addHeader(
        "User-Agent",
        "LyricBuddy/1.0"
      );

      int httpCode =
        https.GET();

      Serial0.print(
        "LRCLIB exact HTTP code: "
      );

      Serial0.println(
        httpCode
      );

      if (
        httpCode == 200
      ) {

        Serial0.print(
          "Free heap before exact parse: "
        );

        Serial0.println(
          ESP.getFreeHeap()
        );

        String response =
          https.getString();

        https.end();

        Serial0.print(
          "LRCLIB exact response bytes: "
        );

        Serial0.println(
          response.length()
        );

        JsonDocument filter;
        filter["syncedLyrics"] = true;

        JsonDocument doc;

        DeserializationError error =
          deserializeJson(
            doc,
            response,
            DeserializationOption::Filter(
              filter
            )
          );

        Serial0.print(
          "LRCLIB exact parse result: "
        );

        Serial0.println(
          error.c_str()
        );

        Serial0.print(
          "Free heap after exact parse: "
        );

        Serial0.println(
          ESP.getFreeHeap()
        );

        if (
          !error
        ) {

          String exactLyrics =
            doc["syncedLyrics"] | "";

          Serial0.print(
            "LRCLIB exact lyric characters: "
          );

          Serial0.println(
            exactLyrics.length()
          );

          if (
            exactLyrics.length() > 0
          ) {

            wordTimedLyricsAvailable = false;
            lineTimedLyricsAvailable = true;

            Serial0.println(
              "LINE MODE READY! Exact LRCLIB match."
            );

            return exactLyrics;
          }

          Serial0.println(
            "Exact LRCLIB match has no synced lyrics."
          );
        }

        else {

          Serial0.print(
            "LRCLIB exact JSON error: "
          );

          Serial0.println(
            error.c_str()
          );
        }
      }

      else {
        https.end();
      }
    }

    else {
      Serial0.println(
        "Could not connect to LRCLIB exact endpoint."
      );
    }
  }


  // ==========================================================
  // 3. EXACT MATCH DID NOT PROVIDE SYNCED LYRICS
  //    SEARCH LRCLIB FOR ALTERNATE MATCHES
  // ==========================================================

  Serial0.println(
    "Trying LRCLIB search fallback..."
  );

  {
    WiFiClientSecure searchClient;
    searchClient.setInsecure();

    HTTPClient https;

    String searchUrl =
      "https://lrclib.net/api/search?track_name=" +
      song +
      "&artist_name=" +
      artist;

    searchUrl.replace(" ", "%20");

    if (
      !https.begin(
        searchClient,
        searchUrl
      )
    ) {

      Serial0.println(
        "Could not connect to LRCLIB search."
      );

      lineTimedLyricsAvailable = false;
      return "";
    }

    https.addHeader(
      "User-Agent",
      "LyricBuddy/1.0"
    );

    int httpCode =
      https.GET();

    Serial0.print(
      "LRCLIB search HTTP code: "
    );

    Serial0.println(
      httpCode
    );

    if (
      httpCode != 200
    ) {

      https.end();

      Serial0.println(
        "LRCLIB search unavailable."
      );

      lineTimedLyricsAvailable = false;
      return "";
    }

    Serial0.print(
      "Free heap before search response: "
    );

    Serial0.println(
      ESP.getFreeHeap()
    );

    // Download the raw search JSON only. We intentionally do NOT
    // build an ArduinoJson document from this large response.
    // Instead, we scan the JSON text for the first non-empty
    // syncedLyrics string and decode only that value.
    String response =
      https.getString();

    https.end();

    Serial0.print(
      "LRCLIB search response bytes: "
    );

    Serial0.println(
      response.length()
    );

    Serial0.print(
      "Free heap after search response: "
    );

    Serial0.println(
      ESP.getFreeHeap()
    );

    String candidateLyrics = "";

    const String key =
      "\"syncedLyrics\"";

    int searchPosition = 0;
    int candidatesChecked = 0;

    while (
      searchPosition < response.length()
    ) {

      int keyPosition =
        response.indexOf(
          key,
          searchPosition
        );

      if (
        keyPosition == -1
      ) {
        break;
      }

      candidatesChecked++;

      int colonPosition =
        response.indexOf(
          ':',
          keyPosition + key.length()
        );

      if (
        colonPosition == -1
      ) {
        break;
      }

      int valuePosition =
        colonPosition + 1;

      while (
        valuePosition < response.length() &&
        (
          response.charAt(valuePosition) == ' ' ||
          response.charAt(valuePosition) == '\\t' ||
          response.charAt(valuePosition) == '\\r' ||
          response.charAt(valuePosition) == '\\n'
        )
      ) {
        valuePosition++;
      }

      // A null value means this candidate has no synced lyrics.
      if (
        response.startsWith(
          "null",
          valuePosition
        )
      ) {
        searchPosition = valuePosition + 4;
        continue;
      }

      // syncedLyrics must be a JSON string.
      if (
        valuePosition >= response.length() ||
        response.charAt(valuePosition) != '"'
      ) {
        searchPosition = valuePosition + 1;
        continue;
      }

      valuePosition++;

      String decoded = "";
      decoded.reserve(12000);

      bool escaped = false;
      bool closedQuote = false;

      for (
        int i = valuePosition;
        i < response.length();
        i++
      ) {

        char c =
          response.charAt(i);

        if (
          escaped
        ) {

          switch (c) {
            case 'n':
              decoded += '\n';
              break;

            case 'r':
              decoded += '\r';
              break;

            case 't':
              decoded += '\t';
              break;

            case 'b':
              decoded += '\b';
              break;

            case 'f':
              decoded += '\f';
              break;

            case '"':
              decoded += '"';
              break;

            case '\\':
              decoded += '\\';
              break;

            case '/':
              decoded += '/';
              break;

            // LRCLIB lyric text is normally ordinary UTF-8.
            // If a JSON unicode escape appears, preserve a safe
            // placeholder instead of corrupting the JSON scan.
            case 'u':
              if (
                i + 4 < response.length()
              ) {
                decoded += '?';
                i += 4;
              }
              break;

            default:
              decoded += c;
              break;
          }

          escaped = false;
          continue;
        }

        if (
          c == '\\'
        ) {
          escaped = true;
          continue;
        }

        if (
          c == '"'
        ) {
          searchPosition = i + 1;
          closedQuote = true;
          break;
        }

        decoded += c;
      }

      if (
        !closedQuote
      ) {
        Serial0.println(
          "LRCLIB search string ended unexpectedly."
        );
        break;
      }

      if (
        decoded.length() > 0
      ) {
        candidateLyrics = decoded;
        break;
      }
    }

    // Release the large search response before returning the
    // lyric String to the rest of the program.
    response = "";

    Serial0.print(
      "LRCLIB search candidates checked: "
    );

    Serial0.println(
      candidatesChecked
    );

    Serial0.print(
      "Free heap after search extraction: "
    );

    Serial0.println(
      ESP.getFreeHeap()
    );

    if (
      candidateLyrics.length() > 0
    ) {

      Serial0.print(
        "Found synced search candidate. Characters: "
      );

      Serial0.println(
        candidateLyrics.length()
      );

      wordTimedLyricsAvailable = false;
      lineTimedLyricsAvailable = true;

      Serial0.println(
        "LINE MODE READY! LRCLIB search fallback."
      );

      return candidateLyrics;
    }

    Serial0.println(
      "LRCLIB search found no synced lyric candidate."
    );

    lineTimedLyricsAvailable = false;
    return "";
  }
}

// ============================================================
// PREPARE DYNAMIC LYRIC LAYOUT
// ============================================================

void prepareLyricLayout() {

  for (
    int i = 0;
    i < MAX_LYRIC_LINES;
    i++
  ) {

    lyricDisplayLines[i] = "";

    lyricX[i] = 0;
    lyricY[i] = 0;
  }


  lyricLineCount = 0;
  lyricTotalCharacters = 0;


  int lyricLength =
    currentLyricLine.length();


  // ==========================================================
  // DYNAMIC TEXT SIZE
  // ==========================================================

  if (
    lyricLength <= 18
  ) {

    lyricTextSize = 4;

  }

  else if (
    lyricLength <= 32
  ) {

    lyricTextSize = 3;

  }

  else {

    lyricTextSize = 2;
  }


  lyricCharWidth =
    6 *
    lyricTextSize;


  lyricCharHeight =
    8 *
    lyricTextSize;


  // ==========================================================
  // CARD WIDTH
  // ==========================================================

  int maxCharsPerLine =
    430 /
    lyricCharWidth;


  if (
    maxCharsPerLine < 1
  ) {

    maxCharsPerLine = 1;
  }


  // ==========================================================
  // WORD WRAP
  // ==========================================================

  String remaining =
    currentLyricLine;


  remaining.trim();


  while (
    remaining.length() > 0 &&
    lyricLineCount <
    MAX_LYRIC_LINES
  ) {

    if (
      remaining.length() <=
      maxCharsPerLine
    ) {

      lyricDisplayLines[
        lyricLineCount
      ] =
        remaining;


      lyricLineCount++;


      remaining = "";


      break;
    }


    int split =
      maxCharsPerLine;


    while (
      split > 0 &&
      remaining.charAt(
        split
      ) != ' '
    ) {

      split--;
    }


    if (
      split <= 0
    ) {

      split =
        maxCharsPerLine;
    }


    lyricDisplayLines[
      lyricLineCount
    ] =
      remaining.substring(
        0,
        split
      );


    lyricDisplayLines[
      lyricLineCount
    ].trim();


    remaining =
      remaining.substring(
        split
      );


    remaining.trim();


    lyricLineCount++;
  }


  // ==========================================================
  // VERY LONG LYRIC
  // ==========================================================

  if (
    remaining.length() > 0 &&
    lyricTextSize > 1
  ) {

    lyricTextSize = 1;

    lyricCharWidth = 6;
    lyricCharHeight = 8;


    maxCharsPerLine =
      430 /
      lyricCharWidth;


    for (
      int i = 0;
      i < MAX_LYRIC_LINES;
      i++
    ) {

      lyricDisplayLines[i] = "";
    }


    lyricLineCount = 0;


    remaining =
      currentLyricLine;


    remaining.trim();


    while (
      remaining.length() > 0 &&
      lyricLineCount <
      MAX_LYRIC_LINES
    ) {

      if (
        remaining.length() <=
        maxCharsPerLine
      ) {

        lyricDisplayLines[
          lyricLineCount
        ] =
          remaining;


        lyricLineCount++;


        remaining = "";


        break;
      }


      int split =
        maxCharsPerLine;


      while (
        split > 0 &&
        remaining.charAt(
          split
        ) != ' '
      ) {

        split--;
      }


      if (
        split <= 0
      ) {

        split =
          maxCharsPerLine;
      }


      lyricDisplayLines[
        lyricLineCount
      ] =
        remaining.substring(
          0,
          split
        );


      lyricDisplayLines[
        lyricLineCount
      ].trim();


      remaining =
        remaining.substring(
          split
        );


      remaining.trim();


      lyricLineCount++;
    }
  }


  // ==========================================================
  // CENTER INSIDE CARD
  // ==========================================================

  int lineGap =
    10;


  if (
    lyricTextSize >= 3
  ) {

    lineGap =
      14;
  }


  int totalHeight =
    (
      lyricLineCount *
      lyricCharHeight
    )
    +
    (
      (lyricLineCount - 1) *
      lineGap
    );


  // Card interior is approximately y 88 -> 252

  int startY =
    88 +
    (
      (164 - totalHeight) /
      2
    );


  // ==========================================================
  // POSITION EACH LINE
  // ==========================================================

  for (
    int i = 0;
    i < lyricLineCount;
    i++
  ) {

    int width =
      lyricDisplayLines[i]
      .length() *
      lyricCharWidth;


    lyricX[i] =
      (480 - width) /
      2;


    if (
      lyricX[i] < 20
    ) {

      lyricX[i] = 20;
    }


    lyricY[i] =
      startY +
      (
        i *
        (
          lyricCharHeight +
          lineGap
        )
      );


    lyricTotalCharacters +=
      lyricDisplayLines[i]
      .length();
  }
}


// ============================================================
// DRAW NEW LYRIC
// ============================================================

void drawLyricScreen() {

  // ==========================================================
  // CLEAR CARD INTERIOR
  // ==========================================================

  tft.fillRoundRect(
    12,
    84,
    456,
    172,
    9,
    UI_CARD
  );


  // Restore green accent

  tft.fillRoundRect(
    205,
    79,
    70,
    3,
    2,
    UI_GREEN
  );


  // ==========================================================
  // PREPARE CURRENT LYRIC
  // ==========================================================

  prepareLyricLayout();


  if (
    lyricLineCount == 0
  ) {

    lastHighlightPixels = -1;

    return;
  }


  // ==========================================================
  // MOVE CURRENT LYRIC SLIGHTLY UP
  // ==========================================================

  for (
    int i = 0;
    i < lyricLineCount;
    i++
  ) {

    lyricY[i] -= 18;
  }


  // ==========================================================
  // DRAW CURRENT LYRIC
  // ==========================================================

  tft.setTextSize(
    lyricTextSize
  );


  tft.setTextColor(
    UI_MUTED,
    UI_CARD
  );


  for (
    int i = 0;
    i < lyricLineCount;
    i++
  ) {

    tft.setCursor(
      lyricX[i],
      lyricY[i]
    );


    tft.print(
      lyricDisplayLines[i]
    );
  }


  // ==========================================================
  // UPCOMING LYRIC
  // ==========================================================

  if (
    nextLyricLine.length() > 0
  ) {

    String upcoming =
      nextLyricLine;


    // Keep the preview compact.

    if (
      upcoming.length() > 60
    ) {

      upcoming =
        upcoming.substring(
          0,
          57
        );

      upcoming += "...";
    }


    // --------------------------------------------------------
    // SMALLER TEXT
    // --------------------------------------------------------

    int upcomingTextSize = 1;

    tft.setTextSize(
      upcomingTextSize
    );


    // Lighter gray than the card,
    // but clearly less important than current lyric.

    uint16_t upcomingColor =
      0xAD55;


    tft.setTextColor(
      upcomingColor,
      UI_CARD
    );


    // --------------------------------------------------------
    // CENTER UPCOMING LYRIC
    // --------------------------------------------------------

    int upcomingWidth =
      upcoming.length() * 6;


    int upcomingX =
      (480 - upcomingWidth) / 2;


    if (
      upcomingX < 20
    ) {

      upcomingX = 20;
    }


    // --------------------------------------------------------
    // SMALL SEPARATOR / GUIDE LINE
    // --------------------------------------------------------

    tft.fillRoundRect(
      215,
      214,
      50,
      2,
      1,
      UI_BORDER
    );


    // --------------------------------------------------------
    // UPCOMING TEXT
    // --------------------------------------------------------

    tft.setCursor(
      upcomingX,
      226
    );


    tft.print(
      upcoming
    );
  }


  lastHighlightPixels = 0;
}


// ============================================================
// SMOOTH KARAOKE HIGHLIGHT
// ============================================================

// ============================================================
// SMOOTH KARAOKE HIGHLIGHT - FORWARD ONLY
// ============================================================

// ============================================================
// DRAW SMOOTH KARAOKE HIGHLIGHT
// ============================================================

void drawKaraokeHighlight(float progress) {

  if (
    currentLyricLine.length() == 0 ||
    lyricLineCount == 0 ||
    lyricTotalCharacters == 0
  ) {
    return;
  }

  // ==========================================================
  // CLAMP PROGRESS
  // ==========================================================

  progress = constrain(progress, 0.0f, 1.0f);

  int totalPixels =
    lyricTotalCharacters *
    lyricCharWidth;

  int targetPixels =
    (int)(totalPixels * progress);

  if (lastHighlightPixels < 0) {
    lastHighlightPixels = 0;
  }

  // Forward only
  if (targetPixels <= lastHighlightPixels) {
    return;
  }

  int oldPixels = lastHighlightPixels;


  // ==========================================================
  // FIND THE NEWLY CROSSED REGION
  // ==========================================================

  int globalPixelStart = 0;

  for (
    int lineIndex = 0;
    lineIndex < lyricLineCount;
    lineIndex++
  ) {

    String line =
      lyricDisplayLines[lineIndex];

    int lineWidth =
      line.length() *
      lyricCharWidth;

    int lineGlobalStart =
      globalPixelStart;

    int lineGlobalEnd =
      lineGlobalStart +
      lineWidth;


    // Does the NEW sweep region touch this line?
    if (
      targetPixels > lineGlobalStart &&
      oldPixels < lineGlobalEnd
    ) {

      int newStart =
        max(oldPixels, lineGlobalStart);

      int newEnd =
        min(targetPixels, lineGlobalEnd);

      int localStart =
        newStart -
        lineGlobalStart;

      int localEnd =
        newEnd -
        lineGlobalStart;


      // ======================================================
      // DETERMINE WHICH CHARACTERS ARE TOUCHED
      // ======================================================

      int firstChar =
        localStart /
        lyricCharWidth;

      int lastChar =
        (localEnd - 1) /
        lyricCharWidth;


      if (firstChar < 0) {
        firstChar = 0;
      }

      if (lastChar >= line.length()) {
        lastChar =
          line.length() - 1;
      }


      // ======================================================
      // DRAW ONLY TOUCHED CHARACTERS
      // ======================================================

      for (
        int charIndex = firstChar;
        charIndex <= lastChar;
        charIndex++
      ) {

        int charStart =
          charIndex *
          lyricCharWidth;

        int charEnd =
          charStart +
          lyricCharWidth;


        // Amount of this character that should now be green
        int greenPixels =
          localEnd -
          charStart;


        if (greenPixels < 0) {
          greenPixels = 0;
        }

        if (greenPixels > lyricCharWidth) {
          greenPixels =
            lyricCharWidth;
        }


        if (greenPixels == 0) {
          continue;
        }


        // ====================================================
        // CHARACTER POSITION ON TFT
        // ====================================================

        int charX =
          lyricX[lineIndex] +
          charStart;

        int charY =
          lyricY[lineIndex];


        // ====================================================
        // FULL CHARACTER
        //
        // Once completely crossed, just draw it normally.
        // ====================================================

        if (
          greenPixels >=
          lyricCharWidth
        ) {

          tft.setTextSize(
            lyricTextSize
          );

          tft.setTextColor(
            UI_GREEN,
            UI_CARD
          );

          tft.setCursor(
            charX,
            charY
          );

          tft.print(
            line.charAt(charIndex)
          );

          continue;
        }


        // ====================================================
        // PARTIAL CHARACTER
        //
        // Build ONE character off-screen.
        // ====================================================

        lyricSprite.fillSprite(
          UI_CARD
        );

        lyricSprite.setTextSize(
          lyricTextSize
        );

        lyricSprite.setTextColor(
          UI_GREEN,
          UI_CARD
        );

        lyricSprite.setCursor(
          0,
          0
        );

        lyricSprite.print(
          line.charAt(charIndex)
        );


        // Push ONLY the part of the character
        // that has been crossed by the sweep.

        lyricSprite.pushSprite(
          charX,
          charY,
          0,
          0,
          greenPixels,
          lyricCharHeight
        );
      }
    }


    globalPixelStart +=
      lineWidth;
  }


  // ==========================================================
  // SAVE POSITION
  // ==========================================================

  lastHighlightPixels =
    targetPixels;
}
// ============================================================
// BOTTOM SONG PROGRESS
// ============================================================

void drawBottomProgress() {

  static int lastFilledWidth = -1;
  static unsigned long lastDisplayedSecond = 999999;

  unsigned long currentPosition =
    spotifyProgressMs;

  if (lastPlaying) {

    currentPosition +=
      millis() -
      progressReceivedAt;
  }


  if (
    spotifyDurationMs > 0 &&
    currentPosition > spotifyDurationMs
  ) {

    currentPosition =
      spotifyDurationMs;
  }


  // ==========================================================
  // PROGRESS BAR SETTINGS
  // ==========================================================

  const int barX = 55;
  const int barY = 291;
  const int barWidth = 370;
  const int barHeight = 4;


  // ==========================================================
  // CALCULATE BAR POSITION
  // ==========================================================

  int filledWidth = 0;


  if (
    spotifyDurationMs > 0
  ) {

    float songProgress =
      (float)currentPosition /
      (float)spotifyDurationMs;


    if (songProgress < 0.0) {
      songProgress = 0.0;
    }


    if (songProgress > 1.0) {
      songProgress = 1.0;
    }


    filledWidth =
      (int)(
        barWidth *
        songProgress
      );
  }


  // ==========================================================
  // FIRST DRAW / SEEK BACKWARD
  // ==========================================================

  if (
    lastFilledWidth == -1 ||
    filledWidth < lastFilledWidth
  ) {

    // Draw complete gray track ONCE

    tft.fillRoundRect(
      barX,
      barY,
      barWidth,
      barHeight,
      2,
      UI_BORDER
    );


    // Draw current green section

    if (
      filledWidth > 0
    ) {

      tft.fillRoundRect(
        barX,
        barY,
        filledWidth,
        barHeight,
        2,
        UI_GREEN
      );
    }


    lastFilledWidth =
      filledWidth;
  }


  // ==========================================================
  // NORMAL FORWARD MOVEMENT
  //
  // ONLY DRAW NEW GREEN PIXELS.
  // NEVER ERASE THE WHOLE BAR.
  // ==========================================================

  else if (
    filledWidth >
    lastFilledWidth
  ) {

    int newPixels =
      filledWidth -
      lastFilledWidth;


    tft.fillRect(
      barX +
      lastFilledWidth,
      barY,
      newPixels,
      barHeight,
      UI_GREEN
    );


    lastFilledWidth =
      filledWidth;
  }


  // ==========================================================
  // TIME DISPLAY
  //
  // Update only when the displayed SECOND changes.
  // ==========================================================

  unsigned long totalSeconds =
    currentPosition /
    1000;


  if (
    totalSeconds !=
    lastDisplayedSecond
  ) {

    lastDisplayedSecond =
      totalSeconds;


    unsigned long currentMinutes =
      totalSeconds /
      60;


    unsigned long currentSeconds =
      totalSeconds %
      60;


    unsigned long durationTotalSeconds =
      spotifyDurationMs /
      1000;


    unsigned long durationMinutes =
      durationTotalSeconds /
      60;


    unsigned long durationSeconds =
      durationTotalSeconds %
      60;


    char currentText[8];

    sprintf(
      currentText,
      "%lu:%02lu",
      currentMinutes,
      currentSeconds
    );


    char durationText[8];

    sprintf(
      durationText,
      "%lu:%02lu",
      durationMinutes,
      durationSeconds
    );


    // Clear ONLY the tiny time areas.

    tft.fillRect(
      10,
      282,
      40,
      14,
      UI_BACKGROUND
    );


    tft.fillRect(
      430,
      282,
      48,
      14,
      UI_BACKGROUND
    );


    tft.setTextSize(
      1
    );


    tft.setTextColor(
      UI_MUTED,
      UI_BACKGROUND
    );


    tft.setCursor(
      14,
      288
    );


    tft.print(
      currentText
    );


    tft.setCursor(
      438,
      288
    );


    tft.print(
      durationText
    );
  }
}


// ============================================================
// UPDATE SYNCHRONIZED LYRICS
// ============================================================

// ==========================================
// GET WORD-SYNCED LYRICS FROM SYNCLRC
// ==========================================


// ============================================================
// UPDATE SYNCHRONIZED LYRICS
// ============================================================

// ============================================================
// UPDATE SYNCHRONIZED LYRICS
// ============================================================

// ============================================================
// UPDATE SYNCHRONIZED LYRICS
// ============================================================

// ============================================================
// UPDATE SYNCHRONIZED LYRICS
// ============================================================
void animateLyricWheel(
  String oldLine,
  String newLine,
  String upcomingLine
) {

  const int spriteX = 20;
  const int spriteY = 96;

  const int spriteWidth = 440;

  // Faster transition
  const int animationDuration = 75;

  // ~60 FPS target
  const int frameTime = 10;


  // ==========================================================
  // PREPARE TEXT
  // ==========================================================

  if (newLine.length() > 55) {
    newLine =
      newLine.substring(0, 52) +
      "...";
  }

  if (upcomingLine.length() > 55) {
    upcomingLine =
      upcomingLine.substring(0, 52) +
      "...";
  }


  unsigned long startTime =
    millis();


  while (true) {

    unsigned long frameStart =
      millis();


    float p =
      (float)(
        frameStart - startTime
      )
      /
      (float)animationDuration;


    if (p > 1.0f) {
      p = 1.0f;
    }


    // ========================================================
    // SMOOTH WHEEL MOTION
    //
    // Smoothstep gives us acceleration at the beginning
    // and deceleration as the lyric reaches the front.
    // ========================================================

    float smooth =
      p * p *
      (3.0f - 2.0f * p);


    // ========================================================
    // POSITIONS
    //
    // New lyric rotates from bottom/back to front.
    //
    // Upcoming lyric follows behind it.
    // ========================================================

    int newY =
      102 -
      (int)(
        60.0f *
        smooth
      );


    int upcomingY =
      148 -
      (int)(
        46.0f *
        smooth
      );


    // ========================================================
    // BUILD FRAME OFF SCREEN
    // ========================================================

    lyricSprite.fillSprite(
      UI_CARD
    );


    // ========================================================
    // NEW CURRENT LYRIC
    // ========================================================

    if (newLine.length() > 0) {

      // Back/lower portion of wheel
      if (p < 0.42f) {

        lyricSprite.setTextSize(1);

        lyricSprite.setTextColor(
          0xAD55,
          UI_CARD
        );


        int width =
          newLine.length() * 6;


        int x =
          (spriteWidth - width) / 2;


        if (x < 0) {
          x = 0;
        }


        lyricSprite.setCursor(
          x,
          newY
        );


        lyricSprite.print(
          newLine
        );
      }

      // Front portion of wheel
      else {

        lyricSprite.setTextSize(2);

        lyricSprite.setTextColor(
          UI_GREEN,
          UI_CARD
        );


        int width =
          newLine.length() * 12;


        int x =
          (spriteWidth - width) / 2;


        if (x < 0) {
          x = 0;
        }


        lyricSprite.setCursor(
          x,
          newY
        );


        lyricSprite.print(
          newLine
        );
      }
    }


    // ========================================================
    // UPCOMING LYRIC
    //
    // No old/read lyric is drawn anymore.
    // Only the NEW upcoming lyric enters from below.
    // ========================================================

    if (
      upcomingLine.length() > 0 &&
      p > 0.20f
    ) {

      lyricSprite.setTextSize(1);

      lyricSprite.setTextColor(
        0xAD55,
        UI_CARD
      );


      int width =
        upcomingLine.length() * 6;


      int x =
        (spriteWidth - width) / 2;


      if (x < 0) {
        x = 0;
      }


      lyricSprite.setCursor(
        x,
        upcomingY
      );


      lyricSprite.print(
        upcomingLine
      );
    }


    // ========================================================
    // PUSH COMPLETE FRAME
    // ========================================================

    lyricSprite.pushSprite(
      spriteX,
      spriteY
    );


    if (p >= 1.0f) {
      break;
    }


    // ========================================================
    // FRAME PACING
    // ========================================================

    unsigned long frameCost =
      millis() - frameStart;


    if (frameCost < frameTime) {

      delay(
        frameTime -
        frameCost
      );
    }
  }


  // ==========================================================
  // FINAL NORMAL LYRIC SCREEN
  // ==========================================================

  drawLyricScreen();
}
void updateLyricsDisplay() {

  if (syncedLyrics.length() == 0) {
    return;
  }


  // ==========================================================
  // PERSISTENT STATE
  // ==========================================================

  static float heldProgress = 0.0;

  static unsigned long heldLineStart = 0;

  static unsigned long lastStablePosition = 0;

  static String lastTrackedSong = "";


  if (lyricClockResetRequested) {

    heldProgress = 0.0;

    heldLineStart = 0;

    lastStablePosition = 0;

    lastTrackedSong = "";

    lyricClockResetRequested = false;
  }


  // ==========================================================
  // BUILD CURRENT SPOTIFY POSITION
  // ==========================================================

  unsigned long currentPosition =
    spotifyProgressMs;


  if (lastPlaying) {

    currentPosition +=
      millis() - progressReceivedAt;
  }


  // Existing lyric timing compensation

  currentPosition += 200;


  // ==========================================================
  // RESET CLOCK PROTECTION WHEN SONG CHANGES
  // ==========================================================

  if (lastSong != lastTrackedSong) {

    lastTrackedSong =
      lastSong;


    lastStablePosition =
      currentPosition;


    heldProgress = 0.0;


    heldLineStart = 0;
  }


  // ==========================================================
  // PREVENT SMALL SPOTIFY CLOCK CORRECTIONS
  // FROM MOVING THE LYRIC CLOCK BACKWARD
  // ==========================================================

  if (lastPlaying) {

    if (
      currentPosition <
      lastStablePosition
    ) {

      currentPosition =
        lastStablePosition;
    }

    else {

      lastStablePosition =
        currentPosition;
    }
  }

  else {

    lastStablePosition =
      currentPosition;
  }


  // ==========================================================
  // FIND CURRENT AND NEXT LYRIC
  // ==========================================================

  String foundCurrentLine = "";

  String foundNextLine = "";


  unsigned long foundCurrentStart = 0;

  unsigned long foundNextStart = 0;


  int position = 0;


  while (
    position <
    syncedLyrics.length()
  ) {

    int lineEnd =
      syncedLyrics.indexOf(
        '\n',
        position
      );


    if (lineEnd == -1) {

      lineEnd =
        syncedLyrics.length();
    }


    String line =
      syncedLyrics.substring(
        position,
        lineEnd
      );


    line.trim();


    if (
      line.length() > 0 &&
      line.charAt(0) == '['
    ) {

      int bracketEnd =
        line.indexOf(']');


      if (bracketEnd != -1) {

        String timestamp =
          line.substring(
            1,
            bracketEnd
          );


        int colon =
          timestamp.indexOf(':');


        if (colon != -1) {

          int minutes =
            timestamp.substring(
              0,
              colon
            ).toInt();


          float seconds =
            timestamp.substring(
              colon + 1
            ).toFloat();


          unsigned long lineTime =
            (minutes * 60000UL) +
            (unsigned long)(
              seconds * 1000.0
            );


          String lyricText =
            line.substring(
              bracketEnd + 1
            );


          lyricText.trim();


          if (
            lineTime <=
            currentPosition
          ) {

            foundCurrentLine =
              lyricText;


            foundCurrentStart =
              lineTime;
          }

          else {

            foundNextLine =
              lyricText;


            foundNextStart =
              lineTime;


            break;
          }
        }
      }
    }


    position =
      lineEnd + 1;
  }


  if (
    foundCurrentLine.length() == 0
  ) {

    return;
  }


  // ==========================================================
  // DETECT REAL LYRIC LINE CHANGE
  // ==========================================================

  bool lineChanged =
    (
      foundCurrentStart !=
      currentLyricStart
    ) ||
    (
      foundCurrentLine !=
      currentLyricLine
    );


  if (lineChanged) {

    previousLyricLine =
      currentLyricLine;


    currentLyricLine =
      foundCurrentLine;


    nextLyricLine =
      foundNextLine;


    currentLyricStart =
      foundCurrentStart;


    nextLyricStart =
      foundNextStart;


    // Reset sweep for genuinely new line

    heldProgress = 0.0;


    heldLineStart =
      foundCurrentStart;


    // Normal lyric screen.
    // No wheel animation.

    drawLyricScreen();


    lastHighlightPixels = 0;
  }

  else {

    nextLyricLine =
      foundNextLine;


    currentLyricStart =
      foundCurrentStart;


    nextLyricStart =
      foundNextStart;
  }


  // ==========================================================
  // LINE STATE SAFETY
  // ==========================================================

  if (
    heldLineStart !=
    currentLyricStart
  ) {

    heldLineStart =
      currentLyricStart;


    heldProgress = 0.0;
  }


  // ==========================================================
  // REQUESTED HIGHLIGHT PROGRESS
  // ==========================================================

  float requestedProgress = 0.0;


  // ==========================================================
  // WORD-TIMED SYNCLRC
  // ==========================================================

  if (
    wordTimedLyricsAvailable &&
    karaokeWordCount > 0
  ) {

    int firstWord = -1;

    int activeWord = -1;


    // ========================================================
    // FIND ACTIVE WORD
    // ========================================================

    for (
      int i = 0;
      i < karaokeWordCount;
      i++
    ) {

      unsigned long wordTime =
        karaokeWords[i].startMs;


      bool belongsToLine =
        wordTime >=
        currentLyricStart;


      if (
        nextLyricStart > 0 &&
        wordTime >=
        nextLyricStart
      ) {

        belongsToLine = false;
      }


      if (belongsToLine) {

        if (firstWord == -1) {

          firstWord = i;
        }


        if (
          wordTime <=
          currentPosition
        ) {

          activeWord = i;
        }

        else {

          break;
        }
      }
    }


    // ========================================================
    // BEFORE FIRST WORD
    // ========================================================

    if (
      firstWord == -1 ||
      activeWord == -1
    ) {

      requestedProgress = 0.0;
    }


    // ========================================================
    // ACTIVE WORD
    // ========================================================

    else {

      unsigned long wordStart =
        karaokeWords[
          activeWord
        ].startMs;


      unsigned long nextWordStart = 0;


      bool nextWordSameLine = false;


      // ======================================================
      // FIND NEXT WORD
      // ======================================================

      if (
        activeWord + 1 <
        karaokeWordCount
      ) {

        unsigned long candidate =
          karaokeWords[
            activeWord + 1
          ].startMs;


        if (
          candidate > wordStart &&
          (
            nextLyricStart == 0 ||
            candidate < nextLyricStart
          )
        ) {

          nextWordStart =
            candidate;


          nextWordSameLine =
            true;
        }
      }


      // ======================================================
      // DETERMINE WORD SWEEP END
      // ======================================================

      unsigned long wordEnd;


      if (nextWordSameLine) {

        unsigned long gap =
          nextWordStart -
          wordStart;


        if (gap <= 1800) {

          wordEnd =
            nextWordStart;
        }

        else {

          // Long pause:
          // finish word, then hold.

          wordEnd =
            wordStart + 1000;
        }
      }

      else {

        if (
          nextLyricStart >
          wordStart
        ) {

          unsigned long gap =
            nextLyricStart -
            wordStart;


          if (gap <= 1800) {

            wordEnd =
              nextLyricStart;
          }

          else {

            wordEnd =
              wordStart + 1000;
          }
        }

        else {

          wordEnd =
            wordStart + 1000;
        }
      }


      if (
        wordEnd <=
        wordStart
      ) {

        wordEnd =
          wordStart + 500;
      }


      // ======================================================
      // WORD PROGRESS
      // ======================================================

      float wordProgress = 0.0;


      if (
        currentPosition >=
        wordEnd
      ) {

        wordProgress = 1.0;
      }

      else if (
        currentPosition >
        wordStart
      ) {

        wordProgress =
          (float)(
            currentPosition -
            wordStart
          )
          /
          (float)(
            wordEnd -
            wordStart
          );
      }


      if (wordProgress < 0.0) {

        wordProgress = 0.0;
      }


      if (wordProgress > 1.0) {

        wordProgress = 1.0;
      }


      // ======================================================
      // WORD -> CHARACTER POSITION
      // ======================================================

      int charStart =
        karaokeWords[
          activeWord
        ].charStart;


      int charEnd =
        karaokeWords[
          activeWord
        ].charEnd;


      float exactCharacter =
        charStart +
        (
          wordProgress *
          (
            charEnd -
            charStart
          )
        );


      if (
        currentLyricLine.length() > 0
      ) {

        requestedProgress =
          exactCharacter /
          (float)
          currentLyricLine.length();
      }
    }
  }


  // ==========================================================
  // LINE-LEVEL FALLBACK
  // ==========================================================

  else if (
    nextLyricStart >
    currentLyricStart
  ) {

    unsigned long elapsed =
      currentPosition -
      currentLyricStart;


    unsigned long duration =
      nextLyricStart -
      currentLyricStart;


    requestedProgress =
      (float)elapsed /
      (float)duration;
  }


  // ==========================================================
  // CLAMP
  // ==========================================================

  if (
    requestedProgress < 0.0
  ) {

    requestedProgress = 0.0;
  }


  if (
    requestedProgress > 1.0
  ) {

    requestedProgress = 1.0;
  }


  // ==========================================================
  // SECOND LAYER OF PROTECTION:
  // HIGHLIGHT ITSELF CAN NEVER MOVE BACKWARD
  // ==========================================================

  if (
    requestedProgress >
    heldProgress
  ) {

    heldProgress =
      requestedProgress;
  }


  // ==========================================================
  // CHOOSE DISPLAY MODE
  // ==========================================================

  if (wordTimedLyricsAvailable) {

    // Karaoke lyrics:
    // use the smooth word-level sweep

    drawKaraokeHighlight(
      heldProgress
    );
  }

  else if (lineTimedLyricsAvailable) {

    // Line-timed lyrics:
    // Highlight the ENTIRE current lyric immediately.
    // No sweep animation.

    tft.setTextSize(
      lyricTextSize
    );


    tft.setTextColor(
      UI_GREEN,
      UI_CARD
    );


    for (
      int i = 0;
      i < lyricLineCount;
      i++
    ) {

      tft.setCursor(
        lyricX[i],
        lyricY[i]
      );


      tft.print(
        lyricDisplayLines[i]
      );
    }


    return;
  }
}


// ============================================================
// SETUP
// ============================================================

bool tftJpegOutput(
  int16_t x,
  int16_t y,
  uint16_t w,
  uint16_t h,
  uint16_t *bitmap
) {

  // Stop drawing if we've gone beyond
  // the bottom of the TFT.

  if (
    y >= tft.height()
  ) {

    return false;
  }


  // Send decoded JPEG pixels directly
  // to the TFT.

  tft.pushImage(
    x,
    y,
    w,
    h,
    bitmap
  );


  return true;
}

void drawAlbumArt(
  String imageUrl
) {

  if (
    imageUrl.length() == 0
  ) {

    Serial0.println(
      "No album art URL."
    );

    return;
  }


  Serial0.println(
    "Downloading album art..."
  );


  WiFiClientSecure client;

  client.setInsecure();

  client.setTimeout(
    10000
  );


  HTTPClient https;


  if (
    !https.begin(
      client,
      imageUrl
    )
  ) {

    Serial0.println(
      "Album art connection failed."
    );

    return;
  }


  int httpCode =
    https.GET();


  Serial0.print(
    "Album art HTTP code: "
  );

  Serial0.println(
    httpCode
  );


  if (
    httpCode != HTTP_CODE_OK
  ) {

    Serial0.println(
      "Album art download failed."
    );

    https.end();

    return;
  }


  int imageSize =
    https.getSize();


  Serial0.print(
    "Album art bytes expected: "
  );

  Serial0.println(
    imageSize
  );


  if (
    imageSize <= 0
  ) {

    Serial0.println(
      "Invalid or unknown album art size."
    );

    https.end();

    return;
  }


  uint8_t *imageBuffer =
    (uint8_t *)malloc(
      imageSize
    );


  if (
    imageBuffer == nullptr
  ) {

    Serial0.println(
      "Not enough memory for album art."
    );

    https.end();

    return;
  }


  WiFiClient *stream =
    https.getStreamPtr();


  int bytesRead =
    stream->readBytes(
      imageBuffer,
      imageSize
    );


  Serial0.print(
    "Album art bytes received: "
  );

  Serial0.println(
    bytesRead
  );


  https.end();


  if (
    bytesRead != imageSize
  ) {

    Serial0.println(
      "Album art download incomplete."
    );

    free(
      imageBuffer
    );

    return;
  }


  // ========================================================
  // CHECK JPEG HEADER
  // ========================================================

  Serial0.print(
    "First JPEG bytes: "
  );


  if (
    imageSize >= 3
  ) {

    Serial0.print(
      imageBuffer[0],
      HEX
    );

    Serial0.print(
      " "
    );

    Serial0.print(
      imageBuffer[1],
      HEX
    );

    Serial0.print(
      " "
    );

    Serial0.println(
      imageBuffer[2],
      HEX
    );
  }


  // A normal JPEG should begin:
  //
  // FF D8 FF


  // ========================================================
  // GET JPEG DIMENSIONS
  // ========================================================

  uint16_t jpegWidth = 0;
  uint16_t jpegHeight = 0;


  JRESULT sizeResult =
    TJpgDec.getJpgSize(
      &jpegWidth,
      &jpegHeight,
      imageBuffer,
      imageSize
    );


  Serial0.print(
    "JPEG size result: "
  );

  Serial0.println(
    sizeResult
  );


  Serial0.print(
    "JPEG dimensions: "
  );

  Serial0.print(
    jpegWidth
  );

  Serial0.print(
    " x "
  );

  Serial0.println(
    jpegHeight
  );


  // ========================================================
  // DRAW JPEG
  // ========================================================

  TJpgDec.setJpgScale(
    1
  );


  int albumX = 208;
  int albumY = 5;


  Serial0.println(
    "Starting JPEG decode..."
  );


  JRESULT decodeResult =
    TJpgDec.drawJpg(
      albumX,
      albumY,
      imageBuffer,
      imageSize
    );


  Serial0.print(
    "JPEG decode result: "
  );

  Serial0.println(
    decodeResult
  );


  free(
    imageBuffer
  );


  Serial0.println(
    "Album art function finished."
  );
}
void setup() {

  // ==========================================================
  // SERIAL
  // ==========================================================

  Serial0.begin(
    115200
  );


  delay(
    1000
  );


  // ==========================================================
  // BUMPER BUTTONS
  // ==========================================================

  pinMode(LEFT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(RIGHT_BUTTON_PIN, INPUT_PULLUP);


  // ==========================================================
  // TFT
  // ==========================================================

  tft.init();


  // Back to the regular 8-bit lyric sprite.

  TJpgDec.setJpgScale(1);

TJpgDec.setSwapBytes(true);

TJpgDec.setCallback(
  tftJpegOutput
);

  lyricSprite.setColorDepth(8);


  if (
    lyricSprite.createSprite(
      440,
      120
    ) == nullptr
  ) {

    Serial0.println(
      "Lyric sprite allocation FAILED!"
    );
  }

  else {

    Serial0.println(
      "Lyric sprite ready!"
    );
  }


  tft.setRotation(
    1
  );


  tft.fillScreen(
    UI_BACKGROUND
  );


  // ==========================================================
  // STARTUP LOGO
  // ==========================================================

  tft.setTextColor(
    UI_GREEN,
    UI_BACKGROUND
  );


  tft.setTextSize(
    3
  );


  tft.setCursor(
    120,
    80
  );


  tft.println(
    "LYRIC BUDDY"
  );


  tft.setTextColor(
    UI_MUTED,
    UI_BACKGROUND
  );


  tft.setTextSize(
    1
  );


  tft.setCursor(
    195,
    130
  );


  tft.println(
    "Connecting..."
  );


  // ==========================================================
  // WIFI
  // ==========================================================

  Serial0.print(
    "Connecting to Wi-Fi"
  );


  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );


  while (
    WiFi.status() !=
    WL_CONNECTED
  ) {

    delay(
      500
    );


    Serial0.print(
      "."
    );
  }


  Serial0.println();


  Serial0.println(
    "Wi-Fi connected!"
  );


  // ==========================================================
  // SPOTIFY TOKEN
  // ==========================================================

  accessToken =
    getSpotifyAccessToken();


  if (
    accessToken.length() == 0
  ) {

    tft.fillScreen(
      UI_BACKGROUND
    );


    tft.setTextColor(
      TFT_RED,
      UI_BACKGROUND
    );


    tft.setTextSize(
      2
    );


    tft.setCursor(
      110,
      140
    );


    tft.println(
      "Spotify login failed"
    );


    return;
  }


  // ==========================================================
  // CREATE MUTEX
  // ==========================================================

  spotifyMutex =
    xSemaphoreCreateMutex();


  if (
    spotifyMutex == NULL
  ) {

    Serial0.println(
      "Could not create Spotify mutex!"
    );


    return;
  }


  // ==========================================================
  // START SPOTIFY BACKGROUND TASK
  // ==========================================================

  BaseType_t taskResult =
    xTaskCreatePinnedToCore(
      spotifyTask,
      "SpotifyTask",
      12288,
      NULL,
      1,
      NULL,
      0
    );


  if (
    taskResult == pdPASS
  ) {

    Serial0.println(
      "Spotify background task started!"
    );
  }

  else {

    Serial0.println(
      "Could not start Spotify task!"
    );
  }
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  // ==========================================================
  // BUMPER BUTTONS
  // ==========================================================

  updateBumperButtons();


  // ==========================================================
  // GET BACKGROUND SPOTIFY DATA
  // ==========================================================

  processSpotifyUpdate();


  // ==========================================================
  // SMOOTH KARAOKE
  // ==========================================================

  updateLyricsDisplay();


  // ==========================================================
  // UPDATE BOTTOM PROGRESS EVERY 500 MS
  // ==========================================================

  static unsigned long
    lastProgressDraw = 0;


  if (
    millis() -
    lastProgressDraw >=
    500
  ) {

    lastProgressDraw =
      millis();


    if (
      lastSong.length() > 0
    ) {

      drawBottomProgress();
    }
  }


  delay(
    10
  );
}