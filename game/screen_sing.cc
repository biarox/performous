#include "screen_sing.hh"

#include "backgrounds.hh"
#include "dancegraph.hh"
#include "database.hh"
#include "engine.hh"
#include "fs.hh"
#include "glutil.hh"
#include "guitargraph.hh"
#include "i18n.hh"
#include "layout_singer.hh"
#include "menu.hh"
#include "screen_players.hh"
#include "songparser.hh"
#include "util.hh"
#include "video.hh"
#include "webcam.hh"

#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <utility>

namespace {
	static const double QUIT_TIMEOUT = 20.0; // Return to songs screen after 20 seconds in score screen

	/// Add a flash message about the state of a config item
	void dispInFlash(ConfigItem& ci) {
		ScreenManager* sm = ScreenManager::getSingletonPtr();
		sm->flashMessage(ci.getShortDesc() + ": " + ci.getValue());
	}
}

ScreenSing::ScreenSing(std::string const& name, Audio& audio, Database& database, Backgrounds& bgs):
	  Screen(name), m_audio(audio), m_database(database), m_backgrounds(bgs),
	  m_selectedTrack(TrackName::LEAD_VOCAL) {
}

void ScreenSing::enter() {
	ScreenManager* sm = ScreenManager::getSingletonPtr();
	// Initialize webcam
	sm->loading(_("Initializing webcam..."), 0.1);
	if (config["graphic/webcam"].b() && Webcam::enabled()) {
		try {
			m_cam.reset(new Webcam(config["graphic/webcamid"].i()));
		} catch (std::exception& e) { std::cout << e.what() << std::endl; };
	}
	// Load video
	sm->loading(_("Loading video..."), 0.2);
	if (!m_song->video.empty() && config["graphic/video"].b()) {
		m_video.reset(new Video(m_song->path + m_song->video, m_song->videoGap));
	}
	boost::ptr_vector<Analyzer>& analyzers = m_audio.analyzers();
	reloadGL();
	// Load song notes
	sm->loading(_("Loading song..."), 0.4);
	if (m_song->loadStatus != Song::FULL) {
		try { SongParser sp(*m_song); }
		catch (SongParserException& e) {
			std::clog << e;
			sm->activateScreen("Songs");
		}
	}
	// Notify about broken tracks
	if (!m_song->b0rked.empty()) sm->dialog(_("Song contains broken tracks!") + std::string("\n\n") + m_song->b0rked);
	// Startup delay for instruments is longer than for singing only
	double setup_delay = (!m_song->hasControllers() ? -1.0 : -5.0);
	m_audio.pause();
	m_audio.playMusic(m_song->music, false, 0.0, setup_delay);
	sm->loading(_("Loading menu..."), 0.7);
	{
		VocalTracks const& tracks = m_song->vocalTracks;
		unsigned players = analyzers.size();
		m_menu.clear();
		m_menu.add(MenuOption(_("Start"), _("Start performing")).call(std::bind(&ScreenSing::setupVocals, this)));
		m_duet = ConfigItem(0);
		if (players == 0) players = 1;  // No mic? We display lyrics anyway
		if (players > 1) { // Duet toggle
			m_duet.addEnum(_("Duet mode"));
			m_duet.addEnum(_("Normal mode"));
			m_menu.add(MenuOption("", _("Switch between duet and regular singing mode")).changer(m_duet));
		}
		for (unsigned player = 0; player < players; ++player) {
			ConfigItem& vocalTrack = m_vocalTracks[player];
			vocalTrack = ConfigItem(0);
			for (VocalTracks::const_iterator it = tracks.begin(); it != tracks.end(); ++it) {
				vocalTrack.addEnum(it->second.name);
			}
			if (tracks.size() > 1 && player % 2) ++vocalTrack;  // Every other player gets the second track
			m_menu.add(MenuOption("", _("Change vocal track")).changer(vocalTrack));
		}
		m_menu.add(MenuOption(_("Quit"), _("Exit to song browser")).screen("Songs"));
		m_menu.open();
		if (tracks.size() <= 1) setupVocals();  // No duet menu
	}
	sm->showLogo(false);
	sm->loading(_("Loading complete"), 1.0);
}

void ScreenSing::setupVocals() {
	if (!m_song->vocalTracks.empty()) {
		m_layout_singer.clear();
		Engine::VocalTrackPtrs selectedTracks;
		boost::ptr_vector<Analyzer>& analyzers = m_audio.analyzers();
		unsigned players = (m_duet.i() == 0 ? (analyzers.empty() ? 1 : analyzers.size()) : 1);  // Always at least 1; number of mics if in duet mode
		std::set<VocalTrack*> shownTracks;  // Tracks to be included in layout_singer (stored by name for proper sorting and merging duplicates)
		for (unsigned player = 0; player < players; ++player) {
			VocalTrack* vocal = &m_song->getVocalTrack(m_vocalTracks[player].i());
			selectedTracks.push_back(vocal);
			shownTracks.insert(vocal);
		}
		if (shownTracks.size() > 2) throw std::runtime_error("Too many tracks chosen. Only two vocal tracks can be used simultaneously.");
		for (std::set<VocalTrack*>::iterator it = shownTracks.begin(); it != shownTracks.end(); ++it) {
			m_layout_singer.push_back(new LayoutSinger(**it, m_database, theme));
		}
		// Note: Engine maps tracks with analyzers 1:1. If user doesn't have mics, we still want to have singer layout enabled but without engine...
		if (!analyzers.empty()) m_engine.reset(new Engine(m_audio, selectedTracks, m_database));
	}
	createPauseMenu();
	m_audio.pause(false);
}

void ScreenSing::createPauseMenu() {
	m_menu.clear();
	m_menu.add(MenuOption(_("Resume"), _("Back to performing!")));
	m_menu.add(MenuOption(_("Restart"), _("Start the song\nfrom the beginning")).screen("Sing"));
	m_menu.add(MenuOption(_("Quit"), _("Exit to song browser")).screen("Songs"));
	m_menu.close();
}

void ScreenSing::reloadGL() {
	// Load UI graphics
	theme.reset(new ThemeSing());
	m_menuTheme.reset(new ThemeInstrumentMenu());
	m_pause_icon.reset(new Surface(getThemePath("sing_pause.svg")));
	m_help.reset(new Surface(getThemePath("instrumenthelp.svg")));
	m_progress.reset(new ProgressBar(getThemePath("sing_progressbg.svg"), getThemePath("sing_progressfg.svg"), ProgressBar::HORIZONTAL, 0.01f, 0.01f, true));
	// Load background
	if (!m_song->background.empty()) m_background.reset(new Surface(m_song->path + m_song->background));
}

void ScreenSing::exit() {
	ScreenManager::getSingletonPtr()->controllers.enableEvents(false);
	m_engine.reset();
	m_score_window.reset();
	m_menu.clear();
	m_instruments.clear();
	m_layout_singer.clear();
	m_help.reset();
	m_pause_icon.reset();
	m_cam.reset();
	m_video.reset();
	m_background.reset();
	m_song->dropNotes();
	m_menuTheme.reset();
	theme.reset();
	if (m_audio.isPaused()) m_audio.togglePause();
	ScreenManager::getSingletonPtr()->showLogo();
}



/// Manages the instrument drawing
void ScreenSing::instrumentLayout(double time) {
	if (!m_song->hasControllers()) return;
	int count_alive = 0, count_menu = 0, i = 0;
	// Remove dead instruments and do the counting
	for (Instruments::iterator it = m_instruments.begin(); it != m_instruments.end(); ) {
		if (it->dead()) {
			it = m_instruments.erase(it);
			continue;
		}
		++count_alive;
		if (it->menuOpen()) ++count_menu;
		++it;
	}
	if (count_alive > 0) {
		// Handle pause
		bool shouldPause = count_menu > 0 || m_menu.isOpen();
		if (shouldPause != m_audio.isPaused()) m_audio.togglePause();
	} else if (time < -0.5) {
		// Display help if no-one has joined yet
		ColorTrans c(Color::alpha(clamp(-1.0 - 2.0 * time)));
		m_help->draw();
	}
	double iw = std::min(0.5, 1.0 / count_alive);
	typedef std::pair<unsigned, double> CountSum;
	std::map<std::string, CountSum> volume; // Stream id to (count, sum)
	std::map<std::string, CountSum> pitchbend; // Stream id to (count, sum)
	for (Instruments::iterator it = m_instruments.begin(); it != m_instruments.end(); ++it, ++i) {
		it->engine();
		it->position((0.5 + i - 0.5 * count_alive) * iw, iw); // Do layout stuff
		it->draw(time);
		{
			CountSum& cs = volume[it->getTrack()];
			cs.first++;
			cs.second += it->correctness();
		}{
			CountSum& cs = pitchbend[it->getTrack()];
			cs.first++;
			cs.second += it->getWhammy();
		}
	}
	// Set volume levels (averages of all instruments playing that track)
	// FIXME: There should NOT be gettext calls here!
	for( std::map<std::string,std::string>::iterator it = m_song->music.begin() ; it != m_song->music.end() ; ++it ) {
		double level = 1.0;
		if (volume.find(_(it->first.c_str())) == volume.end()) {
			m_audio.streamFade(it->first, level);
		} else {
			CountSum cs = volume[_(it->first.c_str())];
			if (cs.first > 0) level = cs.second / cs.first;
			if (m_song->music.size() <= 1) level = std::max(0.333, level);
			m_audio.streamFade(it->first, level);
		}
		if (pitchbend.find(_(it->first.c_str())) != pitchbend.end()) {
			CountSum cs = pitchbend[_(it->first.c_str())];
			level = cs.second;
			m_audio.streamBend(it->first, level);
		}
	}
}

void ScreenSing::activateNextScreen()
{
	ScreenManager* sm = ScreenManager::getSingletonPtr();

	m_database.addSong(m_song);
	if (m_database.scores.empty() || !m_database.reachedHiscore(m_song)) {
		// if no highscore reached..
		sm->activateScreen("Songs");
		return;
	}

	// Score window visible -> Enter quits to Players Screen
	Screen* s = sm->getScreen("Players");
	ScreenPlayers* ss = dynamic_cast<ScreenPlayers*> (s);
	assert(ss);
	ss->setSong(m_song);
	sm->activateScreen("Players");
}

void ScreenSing::manageEvent(input::NavEvent const& event) {
	input::NavButton nav = event.button;
	m_quitTimer.setValue(QUIT_TIMEOUT);
	double time = m_audio.getPosition();
	Song::Status status = m_song->status(time);
	// When score window is displayed
	if (m_score_window.get()) {
		if (nav == input::NAV_START || nav == input::NAV_CANCEL) activateNextScreen();
		return;  // The rest are only available when score window is not displayed
	}
	// Instant quit with CANCEL at the very beginning
	if (nav == input::NAV_CANCEL && time < 1.0) {
		ScreenManager::getSingletonPtr()->activateScreen("Songs");
		return;
	}
	// Only pause or esc opens the global menu (instruments have their own menus)
	// TODO: This should probably check if the source is participating as an instrument or not rather than check for its type
	if (event.source.isKeyboard() && (nav == input::NAV_PAUSE || nav == input::NAV_CANCEL) && !m_audio.isPaused() && !m_menu.isOpen()) {
		m_menu.open();
		m_audio.togglePause();
	}
	// Global/singer pause menu navigation
	if (m_menu.isOpen()) {
		if (nav == input::NAV_START) {
			m_menu.action();
			// Did the action close the menu?
			if (!m_menu.isOpen() && m_audio.isPaused()) {
				m_audio.togglePause();
			}
			return;
		}
		else if (nav == input::NAV_LEFT) { m_menu.action(-1); return; }
		else if (nav == input::NAV_RIGHT) { m_menu.action(1); return; }
		else if (nav == input::NAV_DOWN) { m_menu.move(1); return; }
		else if (nav == input::NAV_UP) { m_menu.move(-1); return; }
	}
	// Start button has special functions for skipping things (only in singing for now)
	if (nav == input::NAV_START && m_instruments.empty() && !m_layout_singer.empty() && !m_audio.isPaused()) {
		// Open score dialog early
		if (status == Song::FINISHED) {
			if (m_engine) m_engine->kill(); // Kill the engine thread
			m_score_window.reset(new ScoreWindow(m_instruments, m_database)); // Song finished, but no score window -> show it
		}
		// Skip instrumental breaks
		else if (status == Song::INSTRUMENTAL_BREAK) {
			if (time < 0) m_audio.seek(0.0);
			else {
				// TODO: Instead of calculating here, calculate instrumental breaks right after song loading and store in Song data structures
				double diff = getNaN();
				for (size_t i = 0; i < m_layout_singer.size(); ++i) {
					double d = m_layout_singer[i].lyrics_begin() - 3.0 - time;
					if (!(d > diff)) diff = d;  // Store smallest d in diff (notice NaN handling)
				}
				if (diff > 0.0) m_audio.seek(diff);
			}
		}
	}
}


void ScreenSing::manageEvent(SDL_Event event) {
	double time = m_audio.getPosition();
	int key = event.key.keysym.sym;
	// Ctrl combinations that can be used while performing (not when score dialog is displayed)
	if (event.type == SDL_KEYDOWN && (event.key.keysym.mod & KMOD_CTRL) && !m_score_window.get()) {
		if (key == SDLK_s) m_audio.toggleSynth(m_song->getVocalTrack(m_selectedTrack).notes);
		if (key == SDLK_v) m_audio.streamFade("vocals", event.key.keysym.mod & KMOD_SHIFT ? 1.0 : 0.0);
		if (key == SDLK_k) dispInFlash(++config["game/karaoke_mode"]); // Toggle karaoke mode
		if (key == SDLK_w) dispInFlash(++config["game/pitch"]); // Toggle pitch wave
		// Toggle webcam
		if (key == SDLK_a && Webcam::enabled()) {
			// Initialize if we haven't done that already
			if (!m_cam) { try { m_cam.reset(new Webcam(config["graphic/webcamid"].i())); } catch (...) { }; }
			if (m_cam) { dispInFlash(++config["graphic/webcam"]); m_cam->pause(!config["graphic/webcam"].b()); }
		}
		// Latency settings
		if (key == SDLK_F1) dispInFlash(--config["audio/video_delay"]);
		if (key == SDLK_F2) dispInFlash(++config["audio/video_delay"]);
		if (key == SDLK_F3) dispInFlash(--config["audio/round-trip"]);
		if (key == SDLK_F4) dispInFlash(++config["audio/round-trip"]);
		if (key == SDLK_F5) dispInFlash(--config["audio/controller_delay"]);
		if (key == SDLK_F6) dispInFlash(++config["audio/controller_delay"]);
		bool seekback = false;

		if (m_song->danceTracks.empty()) { // Seeking backwards is currently not permitted for dance songs
			if (key == SDLK_HOME) { m_audio.seekPos(0.0); seekback = true; }
			if (key == SDLK_LEFT) {
				Song::SongSection section("error", 0);
				if (m_song->getPrevSection(m_audio.getPosition(), section)) {
					m_audio.seekPos(section.begin);
					// TODO: display popup with section.name here
					std::cout << section.name << std::endl;
				} else m_audio.seek(-5.0);
				seekback = true;
			}
		}
		if (key == SDLK_RIGHT) {
			Song::SongSection section("error", 0);
			if (m_song->getNextSection(m_audio.getPosition(), section)) {
				m_audio.seekPos(section.begin);
				// TODO: display popup with section.name here
				std::cout << section.name << std::endl;
			} else m_audio.seek(5.0);
		}

		// Some things must be reset after seeking backwards
		if (seekback)
			for (unsigned i = 0; i < m_layout_singer.size(); ++i)
				m_layout_singer[i].reset();
		// Reload current song
		if (key == SDLK_r) {
			exit(); m_song->reload(); enter();
			m_audio.seek(time);
		}
	}
}

namespace {

	const double arMin = 1.33;
	const double arMax = 2.35;

	void fillBG() {
		Dimensions dim(arMin);
		dim.fixedWidth(1.0);
		glutil::VertexArray va;
		va.TexCoord(0,0).Vertex(dim.x1(), dim.y1());
		va.TexCoord(0,0).Vertex(dim.x2(), dim.y1());
		va.TexCoord(0,0).Vertex(dim.x1(), dim.y2());
		va.TexCoord(0,0).Vertex(dim.x2(), dim.y2());
		getShader("texture").bind();
		va.Draw();
	}

}

void ScreenSing::prepare() {
	ScreenManager* sm = ScreenManager::getSingletonPtr();
	double time = m_audio.getPosition();
	// Enable/disable controllers as needed (mostly so that keyboard navigation will not be obstructed).
	sm->controllers.enableEvents(m_song->hasControllers() && !m_menu.isOpen() && !m_score_window.get());
	if (m_video) m_video->prepare(time);
	for (input::DevicePtr dev; sm->controllers.getDevice(dev); ) {
		// Eat all events and see if any are valid for joining
		input::DevType type = input::DEVTYPE_GENERIC;
		std::string msg;
		for (input::Event ev; dev->getEvent(ev);) {
			if (ev.value == 0.0) continue;
			if (dev->type == input::DEVTYPE_DANCEPAD && m_song->hasDance()) {
				if (ev.button == input::DANCEPAD_UP) type = dev->type;
				else msg = dev->source.isKeyboard() ? _("Press UP to join dance!") : _("Step UP to join!");
			}
			else if (dev->type == input::DEVTYPE_GUITAR && m_song->hasGuitars()) {
				if (ev.button == input::GUITAR_GREEN) type = dev->type;
				else if (ev.button != input::GUITAR_WHAMMY && ev.button != input::GUITAR_GODMODE) {
					msg = dev->source.isKeyboard() ? _("Press 1 to join guitar!") : _("Press GREEN to join!");
				}
			}
			else if (dev->type == input::DEVTYPE_DRUMS && m_song->hasDrums()) {
				if (ev.button == input::DRUMS_KICK) type = dev->type;
				else msg = dev->source.isKeyboard() ? _("Press SPACE to join drums!") : _("KICK to join!");
			}
		}
		if (!msg.empty()) sm->flashMessage(msg, 0.0, 0.1, 0.1);
		else if (type == input::DEVTYPE_DANCEPAD) m_instruments.push_back(new DanceGraph(m_audio, *m_song, dev));
		else if (type != input::DEVTYPE_GENERIC) m_instruments.push_back(new GuitarGraph(m_audio, *m_song, dev, m_instruments.size()));
	}

	// Menu mangling
	// We don't allow instrument menus during global menu
	// except for joining, in which case global menu is closed
	if (m_menu.isOpen()) {
		for (Instruments::iterator it = m_instruments.begin(); it != m_instruments.end(); ++it) {
			if (it->joining(time)) m_menu.close(); else it->toggleMenu(0);
		}
	}


}

void ScreenSing::draw() {
	// Get the time in the song
	double length = m_audio.getLength();
	double time = m_audio.getPosition();
	time -= config["audio/video_delay"].f();
	double songPercent = clamp(time / length);

	// Rendering starts
	{
		Transform ft(farTransform());
		double ar = arMax;
		// Background image
		if (!m_background || m_background->empty()) m_background.reset(new Surface(m_backgrounds.getRandom()));
		ar = m_background->dimensions.ar();
		if (ar > arMax || (m_video && ar > arMin)) fillBG();  // Fill white background to avoid black borders
		m_background->draw();
		// Webcam
		if (m_cam && config["graphic/webcam"].b()) m_cam->render();
		// Video
		if (m_video) {
			m_video->render(time); double tmp = m_video->dimensions().ar(); if (tmp > 0.0) ar = tmp;
		}
		// Top/bottom borders
		ar = clamp(ar, arMin, arMax);
		double offset = 0.5 / ar + 0.2;
		theme->bg_bottom.dimensions.fixedWidth(1.0).bottom(offset);
		theme->bg_bottom.draw();
		theme->bg_top.dimensions.fixedWidth(1.0).top(-offset);
		theme->bg_top.draw();
	}

	for (unsigned i = 0; i < m_layout_singer.size(); ++i) m_layout_singer[i].hideLyrics(m_audio.isPaused());

	instrumentLayout(time);

	bool fullSinger = m_instruments.empty() && m_layout_singer.size() <= 1;
	for (unsigned i = 0; i < m_layout_singer.size(); ++i) {
		m_layout_singer[i].draw(time, fullSinger ? LayoutSinger::FULL : (i == 0 ? LayoutSinger::TOP : LayoutSinger::BOTTOM));
	}

	Song::Status status = m_song->status(time);

	// Compute and draw the timer and the progressbar
	{
		unsigned t = clamp(time, 0.0, length);
		m_progress->dimensions.fixedWidth(0.4).left(-0.5).screenTop();
		theme->timer.dimensions.screenTop(0.5 * m_progress->dimensions.h());
		m_progress->draw(songPercent);

		Song::SongSection section("error", 0);
		std::string statustxt;
		if (m_song->getPrevSection(t - 1.0, section)) {
			statustxt = (boost::format("%02u:%02u - %s") % (t / 60) % (t % 60) % section.name).str();
		} else  statustxt = (boost::format("%02u:%02u") % (t / 60) % (t % 60)).str();

		if (!m_score_window.get() && m_instruments.empty() && !m_layout_singer.empty()) {
			if (status == Song::INSTRUMENTAL_BREAK) statustxt += _("   ENTER to skip instrumental break");
			if (status == Song::FINISHED && !config["game/karaoke_mode"].b()) statustxt += _("   Remember to wait for grading!");
		}
		theme->timer.draw(statustxt);
	}

	if (config["game/karaoke_mode"].b()) {
		if (!m_audio.isPlaying()) {
			ScreenManager* sm = ScreenManager::getSingletonPtr();
			sm->activateScreen("Songs");
			return;
		}
	} else {
		if (m_score_window.get()) {
			// Score window has been created (we are near the end)
			if (m_score_window->empty()) {  // No players to display scores for
				if (!m_audio.isPlaying()) { activateNextScreen(); return; }
			} else {  // Window being displayed
				if (m_quitTimer.get() == 0.0 && !m_audio.isPaused()) { activateNextScreen(); return; }
				m_score_window->draw();
			}
		}
		else if (!m_audio.isPlaying() || (status == Song::FINISHED
		  && m_audio.getLength() - time <= (m_song->instrumentTracks.empty() && m_song->danceTracks.empty() ? 3.0 : 0.2) )) {
			// Time to create the score window
			m_quitTimer.setValue(QUIT_TIMEOUT);
			if (m_engine) m_engine->kill(); // kill the engine thread (to avoid consuming memory)
			m_score_window.reset(new ScoreWindow(m_instruments, m_database));
		}
	}

	// Menus on top of everything
	for (Instruments::iterator it = m_instruments.begin(); it != m_instruments.end(); ++it) if (it->menuOpen()) it->drawMenu();
	if (m_menu.isOpen()) drawMenu();
}


void ScreenSing::drawMenu() {
	if (m_menu.empty()) return;
	// Some helper vars
	ThemeInstrumentMenu& th = *m_menuTheme;
	MenuOptions::const_iterator cur = static_cast<MenuOptions::const_iterator>(&m_menu.current());
	double w = m_menu.dimensions.w();
	const float txth = th.option_selected.h();
	const float step = txth * 0.85f;
	const float h = m_menu.getOptions().size() * step + step;
	float y = -h * .5f + step;
	float x = -w * .5f + step;
	// Background
	th.bg.dimensions.middle(0).center(0).stretch(w, h);
	th.bg.draw();
	// Loop through menu items
	w = 0;
	for (MenuOptions::const_iterator it = m_menu.begin(); it != m_menu.end(); ++it) {
		// Pick the font object
		SvgTxtTheme* txt = &th.option_selected;
		if (cur != it) txt = &(th.getCachedOption(it->getName()));
		// Set dimensions and draw
		txt->dimensions.middle(x).center(y);
		txt->draw(it->getName());
		w = std::max(w, txt->w() + 2 * step); // Calculate the widest entry
		y += step;
	}
	if (cur->getComment() != "") {
		th.comment.dimensions.middle(0).screenBottom(-0.12);
		th.comment.draw(cur->getComment());
	}
	m_menu.dimensions.stretch(w, h);
}


ScoreWindow::ScoreWindow(Instruments& instruments, Database& database):
  m_database(database),
  m_pos(0.8, 2.0),
  m_bg(getThemePath("score_window.svg")),
  m_scoreBar(getThemePath("score_bar_bg.svg"), getThemePath("score_bar_fg.svg"), ProgressBar::VERTICAL, 0.0, 0.0, false),
  m_score_text(getThemePath("score_txt.svg")),
  m_score_rank(getThemePath("score_rank.svg"))
{
	ScreenManager::getSingletonPtr()->showLogo();
	m_pos.setTarget(0.0);
	m_database.scores.clear();
	// Singers
	for (std::list<Player>::iterator p = m_database.cur.begin(); p != m_database.cur.end();) {
		ScoreItem item; item.type = input::DEVTYPE_VOCALS;
		item.score = p->getScore();
		if (item.score < 500) { p = m_database.cur.erase(p); continue; } // Dead
		item.track = "Vocals"; // For database
		item.track_simple = "vocals"; // For ScoreWindow
		item.color = Color(p->m_color.r, p->m_color.g, p->m_color.b);

		m_database.scores.push_back(item);
		++p;
	}
	// Instruments
	for (Instruments::iterator it = instruments.begin(); it != instruments.end();) {
		ScoreItem item;
		item.type = it->getGraphType();
		item.score = it->getScore();
		if (item.score < 100) { it = instruments.erase(it); continue; } // Dead
		item.track_simple = it->getTrack();
		item.track = it->getModeId();
		item.track[0] = toupper(item.track[0]); // Capitalize
		if (item.track_simple == TrackName::DRUMS) item.color = Color(0.1, 0.1, 0.1);
		else if (item.track_simple == TrackName::BASS) item.color = Color(0.5, 0.3, 0.1);
		else item.color = Color(1.0, 0.0, 0.0);

		m_database.scores.push_back(item);
		++it;
	}

	if (m_database.scores.empty())
		m_rank = _("No player!");
	else {
		// Determine winner
		ScoreItem winner = *std::min_element(m_database.scores.begin(), m_database.scores.end());  // Note: best score comes first
		int topScore = winner.score;
		// Determine rank
		if (winner.type == input::DEVTYPE_VOCALS) {
			if (topScore > 8000) m_rank = _("Hit singer");
			else if (topScore > 6000) m_rank = _("Lead singer");
			else if (topScore > 4000) m_rank = _("Rising star");
			else if (topScore > 2000) m_rank = _("Amateur");
			else m_rank = _("Tone deaf");
		} else if (winner.type == input::DEVTYPE_DANCEPAD) {
			if (topScore > 8000) m_rank = _("Maniac");
			else if (topScore > 6000) m_rank = _("Hoofer");
			else if (topScore > 4000) m_rank = _("Rising star");
			else if (topScore > 2000) m_rank = _("Amateur");
			else m_rank = _("Loser");
		} else {
			if (topScore > 8000) m_rank = _("Virtuoso");
			else if (topScore > 6000) m_rank = _("Rocker");
			else if (topScore > 4000) m_rank = _("Rising star");
			else if (topScore > 2000) m_rank = _("Amateur");
			else m_rank = _("Tone deaf");
		}
	}
	m_bg.dimensions.middle().center();
}

void ScoreWindow::draw() {
	using namespace glmath;
	Transform trans(translate(vec3(0.0, m_pos.get(), 0.0)));
	m_bg.draw();
	const double spacing = 0.1 + 0.1 / m_database.scores.size();
	unsigned i = 0;

	for (Database::cur_scores_t::const_iterator p = m_database.scores.begin(); p != m_database.scores.end(); ++p, ++i) {
		int score = p->score;
		ColorTrans c(p->color);
		double x = -0.12 + spacing * (0.5 + i - 0.5 * m_database.scores.size());
		m_scoreBar.dimensions.fixedWidth(0.09).middle(x).bottom(0.20);
		m_scoreBar.draw(score / 10000.0);
		m_score_text.render(boost::lexical_cast<std::string>(score));
		m_score_text.dimensions().middle(x).top(0.24).fixedHeight(0.05);
		m_score_text.draw();
		m_score_text.render(p->track_simple);
		m_score_text.dimensions().middle(x).top(0.20).fixedHeight(0.05);
		m_score_text.draw();
	}
	m_score_rank.draw(m_rank);
}

bool ScoreWindow::empty() {
	return m_database.scores.empty();
}
