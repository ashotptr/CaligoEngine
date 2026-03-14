import os
import sys
import json
import time
import requests
import logging
import argparse
from typing import List, Dict, Any, Optional
import hashlib
from dotenv import load_dotenv

try:
    import spotipy
    from spotipy.oauth2 import SpotifyClientCredentials
    import pylast
    import lyricsgenius as genius

except ImportError:
    print("CRITICAL: Missing libraries. Run: pip install spotipy pylast lyricsgenius requests")

    sys.exit(1)

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger("MusicAgent")

load_dotenv()

CONFIG = {
    "spotify_id": os.environ.get("SPOTIFY_CLIENT_ID"),
    "spotify_secret": os.environ.get("SPOTIFY_CLIENT_SECRET"),
    "lastfm_key": os.environ.get("LASTFM_API_KEY"),
    "lastfm_secret": os.environ.get("LASTFM_API_SECRET"),
    "genius_token": os.environ.get("GENIUS_ACCESS_TOKEN"),
    
    "llm_base_url": "https://openrouter.ai/api/v1", 
    "llm_key": "Bearer " + os.environ.get("OPENROUTER_API_KEY"),
    "llm_model": "meta-llama/llama-3.3-70b-instruct", #google/gemini-2.0-flash-exp:free
}

class MusicEngine:
    """The physical layer that interacts with Music APIs."""
    
    def __init__(self):
        self.sp = self._init_spotify()
        self.lfm = self._init_lastfm()
        self.genius = self._init_genius()

    def _init_spotify(self):
        auth_url = "https://accounts.spotify.com/api/token"

        try:
            resp = requests.post(auth_url, data={
                'grant_type': 'client_credentials',
                'client_id': CONFIG['spotify_id'],
                'client_secret': CONFIG['spotify_secret']
            })
            resp.raise_for_status()
            self.spotify_token = resp.json().get('access_token')
            self.spotify_headers = {"Authorization": f"Bearer {self.spotify_token}"}

            print("✅ Spotify Connected (Raw HTTP)")
        except Exception as e:
            logger.error(f"Spotify Auth Failed: {e}")
            self.spotify_token = None

    def _init_lastfm(self):
        self.lfm_root = "https://ws.audioscrobbler.com/2.0/"

        print("✅ Last.fm Configured (Raw HTTP)")

    def _init_genius(self):
        if not CONFIG["genius_token"]: return None
        return genius.Genius(CONFIG["genius_token"], verbose=False, remove_section_headers=True)

    def search_lyrics_vibe(self, query: str, limit: int = 5) -> str:
        if not self.genius: return json.dumps({"status": "error", "message": "Genius API not initialized."})
        
        print(f"    🔍 Genius: Searching lyrics for '{query}'...")
        
        try:
            results = self.genius.search_lyrics(query, per_page=limit)
            
            tracks = []
            if results and 'sections' in results:
                hits = results['sections'][0]['hits']
                for hit in hits:
                    result = hit['result']
                    tracks.append(f"{result['full_title']} (URL: {result['url']})")
            
            if not tracks:
                return json.dumps(["No lyrical matches found."])
                
            return json.dumps(tracks, indent=2)
            
        except Exception as e:
            logger.error(f"Genius Error: {e}")
            
            return json.dumps({"status": "error", "message": str(e)})

    def search_by_tag(self, tag: str, limit: int = 5) -> str:
        params = {
            "method": "tag.gettoptracks",
            "tag": tag,
            "api_key": CONFIG["lastfm_key"],
            "format": "json",
            "limit": limit
        }
        
        try:
            print(f"    🏷️ Last.fm: Fetching top tracks for tag '{tag}'...")
            resp = requests.get(self.lfm_root, params=params)
            data = resp.json()
            
            if 'tracks' not in data:
                return json.dumps([f"No tracks found for tag: {tag}"])
                
            results = []

            for track in data['tracks']['track']:
                artist = track['artist']['name']
                name = track['name']
                url = track['url']
                results.append(f"{artist} - {name} ({url})")
                
            return json.dumps(results, indent=2)
            
        except Exception as e:
            return json.dumps({"status": "error", "message": str(e)})

    def get_recommendations(self, song_title: str, artist_name: str = None, limit: int = 5) -> str:
        """
        Robust recommendation engine that uses structured inputs (song + artist)
        to find the correct seed track before asking for similar music.
        """
        try:
            display_str = f"'{song_title}' by '{artist_name}'" if artist_name else f"'{song_title}'"

            print(f"    🎵 Last.fm: Searching for {display_str}...")
            
            params_search = {
                "method": "track.search",
                "track": song_title,
                "api_key": CONFIG["lastfm_key"],
                "format": "json",
                "limit": 3
            }

            if artist_name:
                params_search["artist"] = artist_name

            s_resp = requests.get(self.lfm_root, params=params_search)
            s_data = s_resp.json()
            
            track_matches = s_data.get('results', {}).get('trackmatches', {}).get('track', [])
            
            if not track_matches:
                return json.dumps([f"Could not find track {display_str} on Last.fm."])
            
            seed_track = track_matches[0]
            lfm_artist = seed_track['artist']
            lfm_track = seed_track['name']
            lfm_url = seed_track.get('url', 'No URL')
            
            print(f"    ✅ Found seed: {lfm_track} by {lfm_artist}")
            
            params_sim = {
                "method": "track.getsimilar",
                "artist": lfm_artist,
                "track": lfm_track,
                "api_key": CONFIG["lastfm_key"],
                "format": "json",
                "limit": limit
            }
            sim_resp = requests.get(self.lfm_root, params=params_sim)
            sim_data = sim_resp.json()
            
            results = []

            for track in sim_data.get('similartracks', {}).get('track', []):
                name = track['name']
                artist = track['artist']['name']
                url = track.get('url', '#')
                results.append(f"{name} by {artist} ({url})")
                
            if not results:
                return json.dumps(["No similar tracks found."])

            return json.dumps(results, indent=2)

        except Exception as e:
             return json.dumps({"status": "error", "message": f"Recommendation Engine Failure: {e}"})

class MemoryStream:
    def __init__(self, db_file="agent_memory.json"):
        self.db_file = db_file
        self.short_term = []
        self.long_term = self._load_db()

    def _load_db(self):
        if os.path.exists(self.db_file):
            try:
                with open(self.db_file, 'r') as f: return json.load(f)
            except: return []
        return []

    def save_memory(self, user_query: str, insight: str):
        entry = {
            "timestamp": time.time(),
            "query": user_query,
            "insight": insight
        }

        self.long_term.append(entry)

        with open(self.db_file, 'w') as f:
            json.dump(self.long_term, f, indent=2)

    def add_turn(self, role: str, content: str):
        self.short_term.append({"role": role, "content": content})

    def get_context(self) -> List[Dict]:
        recent_insights = [m['insight'] for m in self.long_term[-3:]]
        context_str = "User History/Preferences:\n" + "\n".join(recent_insights) if recent_insights else ""

        return [
            {"role": "system", "content": SYSTEM_PROMPT.replace("{{MEMORY}}", context_str)},
            *self.short_term
        ]

SYSTEM_PROMPT = """
You are the Omniscient Music Agent (Version 2.5). 
You help users find music using semantic understanding, cultural tags, and audio analysis.

{{MEMORY}}

AVAILABLE TOOLS & LOGIC:

1. `search_by_tag(tag)`
   - **Mechanism:** Queries a "folksonomy" database (Last.fm) where millions of users tag music based on how it *feels*.
   - **Best For:** Abstract concepts, Vibes, Moods, Activities, Aesthetics, and Genres.
   - **Trigger:** Use this when the user describes the *context* or *atmosphere* (e.g., "music for coding," "sad vibe," "80s style").
   - **Input Tip:** Convert user descriptions into standardized tags (e.g., "working out" -> "gym" or "high energy").

2. `search_lyrics_vibe(query)`
   - **Mechanism:** Performs a semantic search on the actual text/words of songs (Genius).
   - **Best For:** Concrete Topics, Narratives, Specific Objects, or Storytelling.
   - **Trigger:** Use this when the user describes the *content* or *subject matter* (e.g., "songs about history," "mentioning red cars," "story about a criminal").
   - **Input Tip:** Use concise keywords related to the topic.

3. `get_recommendations(song_title, artist_name)`
   - **Mechanism:** Analyzes audio similarity and listener overlap.
   - **Best For:** Finding "more of the same" or expanding from a specific seed.
   - **Trigger:** Use this ONLY when the user explicitly provides a reference track or artist (e.g., "Songs like X").

INSTRUCTIONS (ReAct Pattern):
1. PLAN: Analyze the user's request type:
   - Is it **Atmospheric** (Context/Mood)? -> Use `search_by_tag`.
   - Is it **Thematic** (Content/Words)? -> Use `search_lyrics_vibe`.
   - Is it **Referential** (Like X)? -> Use `get_recommendations`.
   - Is it **Hybrid**? -> Prioritize `search_by_tag` first.
   
2. THINK: Determine the correct tool and parameters. Return a JSON object for the tool call.
3. REFLECT: If a tool returns poor or empty results, analyze WHY and try the ALTERNATIVE tool.
4. FINAL: Once you have data, summarize it for the user obeying formatting rules. You MUST include the URL provided by the tool in your final answer (e.g. from Last.fm or Genius).

OUTPUT FORMAT (Strict JSON):
You must output ONLY a JSON object. Do not output markdown code blocks.

[
  {
    "thought": "Internal reasoning about the request nature and tool selection.",
    "tool": "tool_name_here",
    "args": { "arg_name": "value" }
  }
]

OR (Final Answer):

[
  {
    "thought": "I have collected sufficient data.",
    "action": "final_answer",
    "content": [
       {"artist": "Artist Name", "title": "Song Title", "url": "https://..." },
       {"artist": "Artist Name", "title": "Song Title", "url": "https://..." }
    ]
  }
]
"""

class MusicAgent:
    def __init__(self):
        raw_id = os.environ.get('HTTP_X_SESSION_ID', 'default_user')
        
        safe_id = hashlib.md5(raw_id.encode()).hexdigest()
        
        mem_dir = "user_memories"
        os.makedirs(mem_dir, exist_ok=True)
        
        unique_db_path = os.path.join(mem_dir, f"memory_{safe_id}.json")

        self.engine = MusicEngine()
        
        self.memory = MemoryStream(db_file=unique_db_path) 
        
        self.tools = {
            "search_lyrics_vibe": self.engine.search_lyrics_vibe,
            "search_by_tag": self.engine.search_by_tag,
            "get_recommendations": self.engine.get_recommendations
        }

    def _call_llm(self, messages: List[Dict]) -> Dict:
        headers = {
            "Authorization": CONFIG['llm_key'],
            "Content-Type": "application/json",
            "HTTP-Referer": "https://music-agent.local", 
        }
        data = {
            "model": CONFIG["llm_model"],
            "messages": messages,
            "temperature": 0.2,
            "response_format": {"type": "json_object"}
        }
        
        try:
            resp = requests.post(
                f"{CONFIG['llm_base_url']}/chat/completions", 
                headers=headers, 
                json=data
            )

            resp.raise_for_status()
            content = resp.json()['choices'][0]['message']['content']
            
            try:
                start = content.find("{")
                end = content.rfind("}") + 1

                return json.loads(content[start:end])
            except json.JSONDecodeError:
                logger.error("LLM failed to output JSON. Retrying...")

                return {"action": "error", "content": "Output was not valid JSON."}
                
        except Exception as e:
            return {"action": "final_answer", "content": f"LLM Connection Error: {e}"}

    def run(self, user_query: str):
        self.memory.add_turn("user", user_query)
        print(f"\n🎧 Agent: Analyzing request: '{user_query}'...")

        max_steps = 5
        for step in range(max_steps):
            context = self.memory.get_context()
            response = self._call_llm(context)
            
            thought = response.get("thought", "No thought provided.")
            action = response.get("action", "")
            
            print(f"    Reflecting: {thought}")

            if action == "final_answer":
                self.memory.save_memory(user_query, f"Provided music for: {user_query}")
                return response.get("content")

            tool_name = response.get("tool")
            tool_args = response.get("args", {})
            
            if tool_name in self.tools:
                print(f"    Acting: Calling {tool_name} with {tool_args}...")
                observation = self.tools[tool_name](**tool_args)
                
                self.memory.add_turn("assistant", json.dumps(response))
                self.memory.add_turn("system", f"Tool Output: {observation}")
                
                try:
                    obs_json = json.loads(observation)
                    if isinstance(obs_json, dict) and obs_json.get("status") == "error":
                        print(f"    ⚠️ Error: {obs_json.get('message')}. Retrying...")
                except json.JSONDecodeError:
                    print("    ⚠️ Error: Tool output was not valid JSON.")
                    
            else:
                self.memory.add_turn("system", f"Error: Tool '{tool_name}' does not exist.")

        return "I tried several steps but couldn't find a perfect match. Try rephrasing?"

if __name__ == "__main__":
    import io
    import contextlib

    parser = argparse.ArgumentParser()
    parser.add_argument("--query", type=str, help="Direct query")
    args = parser.parse_args()

    log_capture = io.StringIO()
    
    handler = logging.StreamHandler(log_capture)
    handler.setFormatter(logging.Formatter('%(asctime)s - %(levelname)s - %(message)s'))

    logging.getLogger().handlers = []
    logging.getLogger().addHandler(handler)
    logging.getLogger().setLevel(logging.INFO)
    
    final_answer = ""
    
    with contextlib.redirect_stdout(log_capture):
        try:
            agent = MusicAgent()

            if args.query:
                final_answer = agent.run(args.query)
            else:
                print("Interactive mode not supported in JSON wrapper.")
        except Exception as e:
            logger.error(f"Execution Error: {str(e)}")
            final_answer = "An internal error occurred. Check logs."

    response_payload = {
        "logs": log_capture.getvalue(),
        "answer": final_answer
    }

    import sys
    sys.stdout.write(json.dumps(response_payload))