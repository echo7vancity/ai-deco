#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/base64.hpp>
#include <Geode/utils/async.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace geode::prelude;

static constexpr float GRID = 30.f;

static std::pair<int, int> toCell(CCPoint p) {
	return { (int)std::round(p.x / GRID), (int)std::round(p.y / GRID) };
}

class $modify(AIDecoUI, EditorUI) {
	struct Fields {
		async::TaskHolder<web::WebResponse> listener;
		async::TaskHolder<Result<std::optional<std::filesystem::path>>> pickHolder;
		std::filesystem::path refImage;
		bool busy = false;
	};

	bool init(LevelEditorLayer* lel) {
		if (!EditorUI::init(lel)) return false;

		auto btn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("AI Deco"),
			this,
			menu_selector(AIDecoUI::onAIDeco)
		);
		btn->setID("ai-deco-button"_spr);

		auto fxBtn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("Beat FX"),
			this,
			menu_selector(AIDecoUI::onBeatFX)
		);
		fxBtn->setID("beat-fx-button"_spr);

		auto imgBtn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("Ref Image"),
			this,
			menu_selector(AIDecoUI::onPickImage)
		);
		imgBtn->setID("ref-image-button"_spr);

		if (auto menu = this->getChildByID("undo-menu")) {
			menu->addChild(imgBtn);
			menu->addChild(btn);
			menu->addChild(fxBtn);
			menu->updateLayout();
		}
		return true;
	}

	void fail(std::string const& msg) {
		m_fields->busy = false;
		FLAlertLayer::create("AI Deco", msg, "OK")->show();
	}

	// ---------------------------------------------------------------
	// Pick a reference image from disk
	// ---------------------------------------------------------------
	void onPickImage(CCObject*) {
		file::FilePickOptions options;
		options.filters = { { "Images", { "*.png", "*.jpg", "*.jpeg", "*.webp" } } };

		m_fields->pickHolder.spawn(
			file::pick(file::PickMode::OpenFile, options),
			[this](Result<std::optional<std::filesystem::path>> result) {
				if (!result.isOk()) return;
				auto opt = result.unwrap();
				if (!opt) return; // user cancelled
				m_fields->refImage = opt.value();
				Notification::create(
					"Reference image: " + m_fields->refImage.filename().string(),
					NotificationIcon::Success
				)->show();
			}
		);
	}

	// ---------------------------------------------------------------
	// AI Deco: send layout (+ optional image) to an AI, place the result
	// ---------------------------------------------------------------
	void onAIDeco(CCObject*) {
		if (m_fields->busy) {
			Notification::create("AI Deco is already working...", NotificationIcon::Loading)->show();
			return;
		}

		auto apiKey = Mod::get()->getSettingValue<std::string>("api-key");
		auto apiUrl = Mod::get()->getSettingValue<std::string>("api-url");
		auto model = Mod::get()->getSettingValue<std::string>("model");
		auto style = Mod::get()->getSettingValue<std::string>("style-prompt");
		auto idStr = Mod::get()->getSettingValue<std::string>("deco-ids");
		auto maxObjects = (size_t)Mod::get()->getSettingValue<int64_t>("max-objects");

		if (apiKey.empty()) {
			FLAlertLayer::create("AI Deco", "Set your API key in the mod settings first.", "OK")->show();
			return;
		}

		std::set<int> allowed;
		for (auto const& part : utils::string::split(idStr, ",")) {
			int id = utils::numFromString<int>(std::string(part)).unwrapOr(0);
			if (id > 0) allowed.insert(id);
		}
		if (allowed.empty()) {
			FLAlertLayer::create("AI Deco", "No valid deco IDs in the mod settings.", "OK")->show();
			return;
		}

		// Use the selection if there is one, otherwise the whole level.
		std::vector<GameObject*> objs;
		auto selected = this->getSelectedObjects();
		if (selected && selected->count() > 0) {
			for (auto o : CCArrayExt<GameObject*>(selected)) objs.push_back(o);
		} else if (m_editorLayer->m_objects) {
			for (auto o : CCArrayExt<GameObject*>(m_editorLayer->m_objects)) objs.push_back(o);
		}
		if (objs.empty()) {
			FLAlertLayer::create("AI Deco", "There are no objects to decorate.", "OK")->show();
			return;
		}

		// Cells already used by gameplay objects, so deco never covers them.
		std::set<std::pair<int, int>> occupied;
		for (auto o : objs) occupied.insert(toCell(o->getPosition()));

		// Send a compact, sampled copy of the layout to keep the request small.
		size_t const sampleLimit = 400;
		size_t step = std::max<size_t>(1, objs.size() / sampleLimit);
		std::ostringstream layout;
		for (size_t i = 0; i < objs.size(); i += step) {
			auto p = objs[i]->getPosition();
			layout << objs[i]->m_objectID << "," << (int)p.x << "," << (int)p.y << ";";
		}

		std::ostringstream idList;
		for (int id : allowed) idList << id << " ";

		std::string sys =
			"You are a Geometry Dash level decorator. Reply with ONLY valid JSON, no markdown: "
			"{\"objects\":[{\"id\":int,\"x\":number,\"y\":number,\"rot\":number,\"scale\":number}]}. "
			"Coordinates are editor units; one block is 30 units. "
			"Only use these object IDs: " + idList.str() + ". "
			"Never place decoration on top of the gameplay objects given. Keep the gameplay clearly readable. "
			"Place at most " + std::to_string(maxObjects) + " objects.";

		std::string usr =
			"Style: " + style + "\n"
			"Gameplay objects as id,x,y; entries (sampled):\n" + layout.str();

		// If a reference image was picked, attach it (needs a vision-capable model).
		matjson::Value userContent = usr;
		if (Mod::get()->getSettingValue<bool>("use-reference-image") && !m_fields->refImage.empty()) {
			auto bytes = utils::file::readBinary(m_fields->refImage);
			if (!bytes) {
				return this->fail("Could not read the reference image.");
			}
			if (bytes.unwrap().size() > 4 * 1024 * 1024) {
				return this->fail("The reference image is over 4 MB. Use a smaller image.");
			}
			auto ext = utils::string::toLower(m_fields->refImage.extension().string());
			std::string mime = (ext == ".png") ? "image/png" : (ext == ".webp") ? "image/webp" : "image/jpeg";
			std::string dataUrl = "data:" + mime + ";base64," + utils::base64::encode(bytes.unwrap());

			sys += " A reference image of the desired decoration is attached. Recreate its look as closely as "
				"possible: match its shapes, density, spacing and layout, using only the allowed object IDs.";

			userContent = std::vector<matjson::Value>{
				matjson::makeObject({ { "type", "text" }, { "text", usr } }),
				matjson::makeObject({
					{ "type", "image_url" },
					{ "image_url", matjson::makeObject({ { "url", dataUrl } }) }
				})
			};
		}

		auto body = matjson::makeObject({
			{ "model", model },
			{ "temperature", 0.8 },
			{ "messages", std::vector<matjson::Value>{
				matjson::makeObject({ { "role", "system" }, { "content", sys } }),
				matjson::makeObject({ { "role", "user" }, { "content", userContent } })
			} }
		});

		web::WebRequest req;
		req.header("Content-Type", "application/json");
		req.header("Authorization", "Bearer " + apiKey);
		req.timeout(std::chrono::seconds(120));
		req.bodyJSON(body);

		m_fields->busy = true;
		Notification::create("AI Deco: asking the AI...", NotificationIcon::Loading)->show();

		m_fields->listener.spawn(
			req.post(apiUrl),
			[this, allowed, occupied, maxObjects](web::WebResponse res) {
				if (!res.ok()) {
					return this->fail(fmt::format("The API returned an error (HTTP {}). Check your key, URL and model.", res.code()));
				}
				auto json = res.json().unwrapOr(matjson::Value());
				if (!json.isObject() || !json.contains("choices")) {
					return this->fail("Unexpected API response format.");
				}
				std::string text = json["choices"][0]["message"]["content"].asString().unwrapOr("");

				// Trim anything outside the outermost braces (markdown fences etc.)
				auto a = text.find('{');
				auto b = text.rfind('}');
				if (a == std::string::npos || b == std::string::npos || b < a) {
					return this->fail("The AI did not return JSON. Try again or use a stronger model.");
				}
				auto parsed = matjson::parse(text.substr(a, b - a + 1));
				if (!parsed) return this->fail("Could not parse the AI's JSON. Try again.");

				auto arr = (*parsed)["objects"].asArray();
				if (!arr) return this->fail("The AI's JSON had no objects list.");

				std::set<std::pair<int, int>> used = occupied;
				size_t placed = 0;
				for (auto& item : arr.unwrap()) {
					if (placed >= maxObjects) break;
					int id = item["id"].asInt().unwrapOr(0);
					if (!allowed.count(id)) continue;

					CCPoint pos = {
						(float)item["x"].asDouble().unwrapOr(0.0),
						(float)item["y"].asDouble().unwrapOr(0.0)
					};
					auto cell = toCell(pos);
					if (used.count(cell)) continue; // keep gameplay readable

					auto deco = m_editorLayer->createObject(id, pos, false);
					if (!deco) continue;

					deco->setRotation((float)item["rot"].asDouble().unwrapOr(0.0));
					float s = std::clamp((float)item["scale"].asDouble().unwrapOr(1.0), 0.2f, 3.0f);
					deco->updateCustomScaleX(s);
					deco->updateCustomScaleY(s);

					used.insert(cell);
					placed++;
				}

				m_fields->busy = false;
				Notification::create(fmt::format("AI Deco placed {} objects", placed), NotificationIcon::Success)->show();
			}
		);
	}

	// ---------------------------------------------------------------
	// Beat FX: pulse (flash) and shake triggers on every beat
	// ---------------------------------------------------------------
	void onBeatFX(CCObject*) {
		auto mod = Mod::get();
		double bpm = mod->getSettingValue<double>("bpm");
		double offset = mod->getSettingValue<double>("beat-offset");
		int64_t pulseEvery = mod->getSettingValue<int64_t>("pulse-every-beats");
		int64_t shakeEvery = mod->getSettingValue<int64_t>("shake-every-beats");
		int64_t pulseChannel = mod->getSettingValue<int64_t>("pulse-channel");
		auto colorStr = mod->getSettingValue<std::string>("pulse-color");
		double pulseFade = mod->getSettingValue<double>("pulse-fade");
		double shakeStrength = mod->getSettingValue<double>("shake-strength");
		int64_t startSpeed = std::clamp<int64_t>(mod->getSettingValue<int64_t>("start-speed"), 0, 4);
		int64_t trigY = mod->getSettingValue<int64_t>("trigger-y");

		if (bpm <= 0) {
			FLAlertLayer::create("Beat FX", "Set a BPM greater than 0 in the mod settings.", "OK")->show();
			return;
		}
		if (pulseEvery <= 0 && shakeEvery <= 0) {
			FLAlertLayer::create("Beat FX", "Both pulses and shakes are turned off in the settings.", "OK")->show();
			return;
		}

		int r = 255, g = 255, b = 255;
		auto parts = utils::string::split(colorStr, ",");
		if (parts.size() == 3) {
			r = std::clamp(utils::numFromString<int>(std::string(parts[0])).unwrapOr(255), 0, 255);
			g = std::clamp(utils::numFromString<int>(std::string(parts[1])).unwrapOr(255), 0, 255);
			b = std::clamp(utils::numFromString<int>(std::string(parts[2])).unwrapOr(255), 0, 255);
		}

		// Find the end of the level and every speed portal, so beats land at the right x.
		static const double speeds[5] = { 251.16, 311.58, 387.42, 468.0, 576.0 }; // units per second
		std::vector<std::pair<double, int>> portals;
		double maxX = 0;
		if (m_editorLayer->m_objects) {
			for (auto o : CCArrayExt<GameObject*>(m_editorLayer->m_objects)) {
				double x = o->getPositionX();
				maxX = std::max(maxX, x);
				switch (o->m_objectID) {
					case 200: portals.push_back({ x, 0 }); break;
					case 201: portals.push_back({ x, 1 }); break;
					case 202: portals.push_back({ x, 2 }); break;
					case 203: portals.push_back({ x, 3 }); break;
					case 1334: portals.push_back({ x, 4 }); break;
					default: break;
				}
			}
		}
		if (maxX <= 0) {
			FLAlertLayer::create("Beat FX", "The level has no objects to sync to.", "OK")->show();
			return;
		}
		std::sort(portals.begin(), portals.end());

		struct Seg { double x, t, speed; };
		std::vector<Seg> segs;
		segs.push_back({ 0.0, 0.0, speeds[startSpeed] });
		for (auto const& [px, idx] : portals) {
			auto const& last = segs.back();
			double t = last.t + (px - last.x) / last.speed;
			segs.push_back({ px, t, speeds[idx] });
		}
		auto xAtTime = [&](double t) {
			Seg const* s = &segs[0];
			for (auto const& seg : segs) {
				if (seg.t <= t) s = &seg;
				else break;
			}
			return s->x + (t - s->t) * s->speed;
		};

		// Build the triggers as a level string (stable property keys across GD versions).
		double beatLen = 60.0 / bpm;
		std::ostringstream str;
		size_t count = 0;
		for (int64_t beat = 0; beat < 20000 && count < 3000; beat++) {
			double x = xAtTime(offset + beat * beatLen);
			if (x < 0) continue;
			if (x > maxX) break;

			if (pulseEvery > 0 && beat % pulseEvery == 0) {
				str << "1,1006,2," << (int)x << ",3," << trigY
					<< ",7," << r << ",8," << g << ",9," << b
					<< ",45,0,46,0,47," << pulseFade
					<< ",48,0,51," << pulseChannel << ",52,0;";
				count++;
			}
			if (shakeEvery > 0 && beat % shakeEvery == 0) {
				str << "1,1520,2," << (int)x << ",3," << (trigY + 60)
					<< ",75," << shakeStrength << ",84,0,10," << (beatLen * 0.5) << ";";
				count++;
			}
		}

		if (count == 0) {
			FLAlertLayer::create("Beat FX", "No beats landed inside the level. Check BPM and offset.", "OK")->show();
			return;
		}

		m_editorLayer->createObjectsFromString(str.str(), true, true);
		Notification::create(fmt::format("Beat FX placed {} triggers", count), NotificationIcon::Success)->show();
	}
};
