#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_JPEG_Image.H>
#include <FL/Fl_PNG_Image.H>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <curl/easy.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

struct queryField {
  Fl_Input* song;
  Fl_Input* artist;
  Fl_Group* radios;
};

struct DownloadContext {
  std::string title;
  std::string artist;
  std::string album;
  std::string mbid;
  bool is_album;
  json item_data;
};

struct ProgressWindow {
  Fl_Window* win;
  Fl_Progress* bar;
  Fl_Box* status;
};

static size_t wcb(void* contents, size_t size, size_t nmemb, std::string* output) {
  output->append((char*)contents, size * nmemb);
  return size * nmemb;
}

static size_t write_vec_cb(void* contents, size_t size, size_t nmemb, std::vector<unsigned char>* buf) {
  size_t total = size * nmemb;
  auto* bytes = static_cast<unsigned char*>(contents);
  buf->insert(buf->end(), bytes, bytes + total);
  return total;
}

// Get standard user Music directory path
fs::path get_music_directory() {
  const char* home = std::getenv("HOME");
  if (!home) {
    home = std::getenv("USERPROFILE"); // Fallback for Windows
  }
  if (home) {
    fs::path p(home);
    return p / "Music";
  }
  return fs::current_path();
}

// Low-res (front-250) for UI preview, High-res (front-1200) for MP3 tagging
std::vector<unsigned char> fetch_cover_bytes(const std::string& mbid, bool is_release_group, bool high_res = false) {
  std::vector<unsigned char> img_buf;
  std::string size_suffix = high_res ? "front-1200" : "front-250";

  std::string url = is_release_group 
    ? "https://coverartarchive.org/release-group/" + mbid + "/" + size_suffix
    : "https://coverartarchive.org/release/" + mbid + "/" + size_suffix;

  CURL* curl = curl_easy_init();
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_vec_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &img_buf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MusicDL/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
      img_buf.clear();
    }
  }
  return img_buf;
}

std::string fetch(const char* nm, const char* an, int qType) {
  std::string url;

  char* escNm = curl_escape(nm, strlen(nm));
  char* escAN = curl_escape(an, strlen(an));

  switch (qType) {
    case 0:
      if (strlen(an) == 0) {
        url = std::string("https://musicbrainz.org/ws/2/recording?query=") + escNm +
          std::string("&limit=10&fmt=json&inc=releases+cover-art-archive");
        break;
      }
      url = std::string("https://musicbrainz.org/ws/2/recording?query=") + escNm +
        "+AND+artist:" + escAN +
        std::string("&limit=10&fmt=json&inc=releases+cover-art-archive");
      break;

    case 1:
      if (strlen(an) == 0) {
        url = std::string("https://musicbrainz.org/ws/2/release-group?query=") + escNm +
          std::string("&limit=10&fmt=json&type=album&inc=releases+media+cover-art-archive");
        break;
      }
      url = std::string("https://musicbrainz.org/ws/2/release-group?query=") + escNm +
        "+AND+artist:" + escAN +
        std::string("&limit=10&fmt=json&type=album&inc=releases+media+cover-art-archive");
      break;
  }

  curl_free(escAN);
  curl_free(escNm);

  CURL* curl = curl_easy_init();
  std::string response;

  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MusicDL/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
  }

  return response;
}

void sanitize_filename(std::string& name) {
  for (char& c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      c = '_';
    }
  }
}

void apply_file_tags(const std::string& filepath, const std::string& title, const std::string& artist, const std::string& album, const std::vector<unsigned char>& art_bytes) {
  TagLib::MPEG::File file(filepath.c_str());
  if (!file.isValid()) return;

  TagLib::ID3v2::Tag* tag = file.ID3v2Tag(true);
  if (tag) {
    tag->setTitle(TagLib::String(title, TagLib::String::UTF8));
    tag->setArtist(TagLib::String(artist, TagLib::String::UTF8));
    tag->setAlbum(TagLib::String(album, TagLib::String::UTF8));

    if (!art_bytes.empty()) {
      TagLib::ID3v2::AttachedPictureFrame* frame = new TagLib::ID3v2::AttachedPictureFrame();
      frame->setMimeType("image/jpeg");
      frame->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
      frame->setPicture(TagLib::ByteVector((const char*)art_bytes.data(), art_bytes.size()));
      tag->addFrame(frame);
    }

    file.save();
  }
}

void update_progress_ui(void* data) {
  auto* p = static_cast<std::pair<ProgressWindow*, std::pair<float, std::string>>*>(data);
  ProgressWindow* pw = p->first;
  float val = p->second.first;
  std::string msg = p->second.second;

  pw->bar->value(val);
  pw->status->copy_label(msg.c_str());

  if (val >= 100.0f) {
    pw->win->hide();
    delete pw->win;
    delete pw;
  }
  delete p;
}

void dispatch_progress(ProgressWindow* pw, float val, const std::string& msg) {
  auto* payload = new std::pair<ProgressWindow*, std::pair<float, std::string>>(pw, {val, msg});
  Fl::awake(update_progress_ui, payload);
}

void process_single_download(const std::string& title, const std::string& artist, const std::string& album, const std::string& mbid, bool is_rg, ProgressWindow* pw, float start_pct, float end_pct, const fs::path& target_dir) {
  std::string safe_title = title;
  sanitize_filename(safe_title);

  fs::path full_filepath = target_dir / (safe_title + ".mp3");

  dispatch_progress(pw, start_pct, "Downloading: " + title);

  std::string searchQuery = "ytsearch1:\"" + artist + " - " + title + "\"";
  std::string cmd = "yt-dlp -x --audio-format mp3 --audio-quality 0 -o \"" + full_filepath.string() + "\" " + searchQuery;

  int res = std::system(cmd.c_str());

  if (res == 0) {
    dispatch_progress(pw, start_pct + (end_pct - start_pct) * 0.7f, "Fetching 1200px Cover Art: " + title);
    // Uses 1200px artwork for actual MP3 metadata tags
    std::vector<unsigned char> art_bytes = fetch_cover_bytes(mbid, is_rg, true);

    dispatch_progress(pw, start_pct + (end_pct - start_pct) * 0.9f, "Tagging: " + title);
    apply_file_tags(full_filepath.string(), title, artist, album, art_bytes);

    dispatch_progress(pw, end_pct, "Completed: " + title);
  } else {
    dispatch_progress(pw, end_pct, "Failed: " + title);
  }
}

void run_download_thread(DownloadContext ctx, ProgressWindow* pw) {
  fs::path music_dir = get_music_directory();

  if (!ctx.is_album) {
    fs::create_directories(music_dir);
    process_single_download(ctx.title, ctx.artist, ctx.album, ctx.mbid, false, pw, 0.0f, 100.0f, music_dir);
  } else {
    dispatch_progress(pw, 5.0f, "Fetching Album Tracklist...");
    std::string release_group_id = ctx.mbid;
    std::string url = "https://musicbrainz.org/ws/2/release-group/" + release_group_id + "?inc=releases&fmt=json";

    CURL* curl = curl_easy_init();
    std::string json_resp;
    if (curl) {
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wcb);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &json_resp);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "MusicDL/1.0");
      curl_easy_perform(curl);
      curl_easy_cleanup(curl);
    }

    if (!json_resp.empty()) {
      try {
        json rg_data = json::parse(json_resp);
        if (rg_data.contains("releases") && rg_data["releases"].is_array() && !rg_data["releases"].empty()) {
          std::string release_id = rg_data["releases"][0].value("id", "");

          std::string rel_url = "https://musicbrainz.org/ws/2/release/" + release_id + "?inc=recordings+artist-credits&fmt=json";
          std::string rel_resp;

          CURL* curl_rel = curl_easy_init();
          if (curl_rel) {
            curl_easy_setopt(curl_rel, CURLOPT_URL, rel_url.c_str());
            curl_easy_setopt(curl_rel, CURLOPT_WRITEFUNCTION, wcb);
            curl_easy_setopt(curl_rel, CURLOPT_WRITEDATA, &rel_resp);
            curl_easy_setopt(curl_rel, CURLOPT_USERAGENT, "MusicDL/1.0");
            curl_easy_perform(curl_rel);
            curl_easy_cleanup(curl_rel);
          }

          if (!rel_resp.empty()) {
            json rel_data = json::parse(rel_resp);
            if (rel_data.contains("media") && rel_data["media"].is_array()) {
              struct TrackInfo { std::string title; std::string artist; };
              std::vector<TrackInfo> tracks_to_dl;

              for (const auto& media : rel_data["media"]) {
                if (media.contains("tracks") && media["tracks"].is_array()) {
                  for (const auto& track : media["tracks"]) {
                    std::string track_title = track.value("title", "Unknown");
                    std::string track_artist = ctx.artist;

                    if (track.contains("artist-credit") && track["artist-credit"].is_array() && !track["artist-credit"].empty()) {
                      track_artist = track["artist-credit"][0].value("name", ctx.artist);
                    }
                    tracks_to_dl.push_back({track_title, track_artist});
                  }
                }
              }

              std::string folder_artist = ctx.artist;
              std::string folder_album = ctx.title;
              sanitize_filename(folder_artist);
              sanitize_filename(folder_album);

              fs::path album_dir = music_dir / (folder_artist + " - " + folder_album);
              fs::create_directories(album_dir);

              size_t total = tracks_to_dl.size();
              for (size_t i = 0; i < total; ++i) {
                float start_p = 10.0f + (90.0f * i / total);
                float end_p = 10.0f + (90.0f * (i + 1) / total);
                process_single_download(tracks_to_dl[i].title, tracks_to_dl[i].artist, ctx.title, release_id, false, pw, start_p, end_p, album_dir);
              }
            }
          }
        }
      } catch (const std::exception& e) {
        dispatch_progress(pw, 100.0f, "Album Download Error");
      }
    }
  }
}

void download_cb(Fl_Widget*, void* data) {
  DownloadContext* ctx = static_cast<DownloadContext*>(data);

  Fl_Window* win = new Fl_Window(400, 100, "Downloading...");
  Fl_Progress* bar = new Fl_Progress(20, 20, 360, 30);
  bar->minimum(0);
  bar->maximum(100);
  bar->value(0);

  Fl_Box* status = new Fl_Box(20, 60, 360, 25, "Starting download...");
  status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  win->end();
  win->show();

  ProgressWindow* pw = new ProgressWindow{win, bar, status};

  std::thread t(run_download_thread, *ctx, pw);
  t.detach();
}

struct ImageUpdateData {
  Fl_Box* cbox;
  std::vector<unsigned char> rawBytes;
};

void apply_image_ui(void* data) {
  auto* img_data = static_cast<ImageUpdateData*>(data);

  if (!img_data->rawBytes.empty()) {
    Fl_Image* img = new Fl_JPEG_Image("cv", img_data->rawBytes.data(), img_data->rawBytes.size());

    if (img->w() <= 0 || img->h() <= 0) {
      delete img;
      img = new Fl_PNG_Image("cv", img_data->rawBytes.data(), img_data->rawBytes.size());
    }

    if (img->w() > 0 && img->h() > 0) {
      Fl_Image* scaled = img->copy(img_data->cbox->w(), img_data->cbox->h());
      delete img;
      img_data->cbox->image(scaled);
      img_data->cbox->redraw();
    } else {
      delete img;
    }
  }
  delete img_data;
}

void fetch_cover_art_thread(Fl_Box* cbox, std::string mbid, bool is_rg) {
  // Uses 250px preview images for fast UI loading
  std::vector<unsigned char> rawBytes = fetch_cover_bytes(mbid, is_rg, false);
  if (!rawBytes.empty()) {
    ImageUpdateData* payload = new ImageUpdateData{cbox, rawBytes};
    Fl::awake(apply_image_ui, payload);
  }
}

class resBox : public Fl_Group {
  Fl_Box* cbox;
  Fl_Box* nmlbl;
  Fl_Box* anlbl;
  Fl_Box* inflbl;
  Fl_Button* dlbtn;
  DownloadContext dlCtx;

  public:
  resBox(int x, int y, int w, int h, const json& item, int type)
    : Fl_Group(x, y, w, h) {
      begin();

      cbox = new Fl_Box(x + 5, y + 5, h - 10, h - 10);
      cbox->box(FL_THIN_UP_BOX);
      cbox->color(FL_BACKGROUND_COLOR);

      nmlbl = new Fl_Box(x + h + 10, y + 5, w - h - 60, 30);
      std::string itemTitle = item.value("title", "Unknown");
      nmlbl->copy_label(itemTitle.c_str());
      nmlbl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
      nmlbl->labelfont(FL_HELVETICA_BOLD);
      nmlbl->labelsize(20);

      std::string artistName = "Unknown";

      if (item.contains("artist-credit") && item["artist-credit"].is_array()) {
        artistName = "";
        for (const auto& credit : item["artist-credit"]) {
          if (credit.contains("name") && credit["name"].is_string()) {
            artistName += credit["name"].get<std::string>();
          }
          if (credit.contains("joinphrase") && credit["joinphrase"].is_string()) {
            artistName += credit["joinphrase"].get<std::string>();
          }
        }
        if (artistName.empty()) {
          artistName = "Unknown";
        }
      }

      anlbl = new Fl_Box(x + h + 10, y + 40, w - h - 60, 20);
      anlbl->copy_label(artistName.c_str());
      anlbl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
      anlbl->labelfont(FL_HELVETICA);
      anlbl->labelsize(14);

      // Extract release year
      std::string year = "????";
      if (item.contains("first-release-date") && item["first-release-date"].is_string()) {
        std::string date_str = item["first-release-date"].get<std::string>();
        if (date_str.length() >= 4) {
          year = date_str.substr(0, 4);
        }
      } else if (item.contains("releases") && item["releases"].is_array() && !item["releases"].empty()) {
        if (item["releases"][0].contains("date") && item["releases"][0]["date"].is_string()) {
          std::string date_str = item["releases"][0]["date"].get<std::string>();
          if (date_str.length() >= 4) {
            year = date_str.substr(0, 4);
          }
        }
      }

      std::string meta = "";

      switch (type) {
        case 0: { // Song
                  int ms = item.value("length", 0);
                  int min = ms / 60000;
                  int sec = (ms % 60000) / 1000;
                  char buf[16];
                  snprintf(buf, sizeof(buf), "%d:%02d", min, sec);
                  meta = year + " \xE2\x80\xA2 " + std::string(buf);
                  break;
                }
        case 1: { // Album
                  int tracks = 0;

                  // Primary track count resolution logic
                  if (item.contains("releases") && item["releases"].is_array()) {
                    for (const auto& rel : item["releases"]) {
                      if (rel.contains("track-count") && rel["track-count"].is_number()) {
                        tracks = rel["track-count"].get<int>();
                        if (tracks > 0) break;
                      }
                      if (rel.contains("media") && rel["media"].is_array()) {
                        for (const auto& med : rel["media"]) {
                          if (med.contains("track-count") && med["track-count"].is_number()) {
                            tracks += med["track-count"].get<int>();
                          } else if (med.contains("track_count") && med["track_count"].is_number()) {
                            tracks += med["track_count"].get<int>();
                          }
                        }
                        if (tracks > 0) break;
                      }
                    }
                  }

                  if (tracks == 0) {
                    tracks = item.value("count", item.value("track-count", 0));
                  }

                  meta = year + " \xE2\x80\xA2 " + std::to_string(tracks) + " tracks";
                  break;
                }
      }

      inflbl = new Fl_Box(x + h + 10, y + 65, w - h - 60, 20);
      inflbl->copy_label(meta.c_str());
      inflbl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
      inflbl->labelfont(FL_HELVETICA_ITALIC);
      inflbl->labelsize(12);

      dlbtn = new Fl_Button(x + w - h - 15, y + 5, h - 10, h - 10, "+");
      dlbtn->box(FL_THIN_UP_BOX);
      dlbtn->labelsize(24);

      std::string mbid = "";
      bool is_rg = (type == 1);

      if (type == 0) {
        if (item.contains("releases") && item["releases"].is_array() && !item["releases"].empty()) {
          mbid = item["releases"][0].value("id", "");
        }
      } else {
        mbid = item.value("id", "");
      }

      dlCtx.title = itemTitle;
      dlCtx.artist = artistName;
      dlCtx.album = (type == 1) ? itemTitle : "";
      dlCtx.mbid = mbid;
      dlCtx.is_album = is_rg;
      dlCtx.item_data = item;

      dlbtn->callback(download_cb, &dlCtx);

      if (!mbid.empty()) {
        std::thread(fetch_cover_art_thread, cbox, mbid, is_rg).detach();
      }

      end();
      resizable(this);
    }
};

Fl_Scroll* resScroll = nullptr;

void search(Fl_Widget*, void* data) {
  queryField* fields = static_cast<queryField*>(data);

  const char* sn = fields->song->value();
  const char* an = fields->artist->value();
  int qType = 0;

  for (int i = 0; i < fields->radios->children(); i++) {
    if (static_cast<Fl_Round_Button*>(fields->radios->child(i))->value() == 1) {
      qType = i;
      break;
    }
  }

  std::string jstr = fetch(sn, an, qType);
  if (jstr.empty()) return;

  resScroll->clear();

  // Reset scroll position to top-left (0,0) before adding new widgets
  resScroll->scroll_to(0, 0);

  try {
    json root = json::parse(jstr);
    bool album = (qType == 1);
    std::string arrKey = album ? "release-groups" : "recordings";

    int yp = 5;
    int bh = 100;
    int sw = resScroll->w() - 20;

    if (root.contains(arrKey)) {
      for (const auto& item : root[arrKey]) {
        resScroll->begin();
        new resBox(0, yp, sw, bh, item, static_cast<int>(album));
        resScroll->end();
        yp += bh + 5;
      }
    }

    resScroll->init_sizes();
    resScroll->redraw();
  } catch (const std::exception& e) {
    std::cout << "JSON parse error: " << e.what() << std::endl;
  }
}
int main(int argc, char** argv) {
  Fl::lock();
  curl_global_init(CURL_GLOBAL_DEFAULT);

  queryField* mainField = new queryField{nullptr, nullptr, nullptr};

  Fl_Window* window = new Fl_Window(1000, 800, "MusicDL");
  Fl_Group* group = new Fl_Group(0, 0, 1000, 800);

  Fl_Input* box = new Fl_Input(10, 10, 900, 50);
  box->textsize(28);
  box->textfont(FL_HELVETICA);
  box->when(FL_WHEN_ENTER_KEY_ALWAYS);
  box->callback(search, mainField);
  mainField->song = box;

  Fl_Button* subm = new Fl_Button(920, 10, 70, 50, "->");
  subm->labelsize(28);
  subm->callback(search, mainField);

  Fl_Input* author = new Fl_Input(10, 70, 100, 30);
  author->textsize(16);
  author->textfont(FL_HELVETICA);
  author->when(FL_WHEN_ENTER_KEY_ALWAYS);
  author->callback(search, mainField);
  mainField->artist = author;

  Fl_Group* rbg = new Fl_Group(120, 70, 880, 30);
  Fl_Round_Button* rb1 = new Fl_Round_Button(120, 70, 80, 30, "Song");
  rb1->type(FL_RADIO_BUTTON);
  rb1->value(1);

  Fl_Round_Button* rb2 = new Fl_Round_Button(200, 70, 80, 30, "Album");
  rb2->type(FL_RADIO_BUTTON);
  rbg->end();
  mainField->radios = rbg;

  resScroll = new Fl_Scroll(10, 110, 980, 670);
  resScroll->end();

  group->end();
  window->end();
  window->show(argc, argv);

  int ret = Fl::run();
  curl_global_cleanup();
  return ret;
}
