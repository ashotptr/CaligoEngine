import sys
import os
import subprocess
import shutil
import re
import json
import time
import urllib.request
from mutagen.easyid3 import EasyID3
from mutagen.mp3 import MP3

sys.stdout.reconfigure(line_buffering=True)

try:
    from spotdl.utils.config import get_config
    from spotdl.utils.spotify import SpotifyClient
    from spotdl.types.album import Album
    from spotdl.types.playlist import Playlist
    from spotdl.types.song import Song
    
    try:
        config = get_config()
        SpotifyClient.init(
            client_id=config.get("client_id"),
            client_secret=config.get("client_secret"),
            user_auth=False,
            cache_path=config.get("cache_path"),
            no_cache=config.get("no_cache")
        )
    except Exception as e:
        print(f"WARNING: Spotify Init failed: {e}")

except ImportError:
    print("CRITICAL: SpotDL library not found. Run 'pip install spotdl'")
    sys.exit(1)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
MUSIC_DIR = os.path.join(PROJECT_ROOT, "public_html", "mixtape")
TEMP_DIR = "/tmp/caligo_downloads"
COOKIE_FILE = os.path.join(PROJECT_ROOT, "cookies.txt")

def sanitize_filename(name):
    name = name.replace(' ', '-')
    name = re.sub(r'[^a-zA-Z0-9_.-]', '', name)
    return name.lower()

def get_cover_url(item, songs=None):
    if hasattr(item, 'cover_url') and item.cover_url:
        return item.cover_url
    
    if hasattr(item, 'images') and item.images:
        return item.images[0]['url']
        
    if songs and len(songs) > 0:
        if hasattr(songs[0], 'cover_url') and songs[0].cover_url:
            return songs[0].cover_url
            
    return None

def download_and_ingest(query):
    if not os.path.exists(TEMP_DIR): os.makedirs(TEMP_DIR)
    job_id = str(int(time.time()))
    work_dir = os.path.join(TEMP_DIR, job_id)
    os.makedirs(work_dir)

    print(f"Query: {query}")
    song_queue = [] 
    cover_url = None

    is_url = "http" in query
    
    if is_url:
        print(">> Detected URL. Fetching Spotify Metadata...")
        try:
            if "album" in query:
                item = Album.from_url(query)
                print(f"   Found Album: {item.name}")
                
                cover_url = get_cover_url(item, item.songs)
                for song in item.songs:
                    song_queue.append({
                        "title": song.name,
                        "artist": song.artist,
                        "album": item.name,
                        "search_term": f"{song.artist} - {song.name} Audio"
                    })
            elif "playlist" in query:
                item = Playlist.from_url(query)
                print(f"   Found Playlist: {item.name}")
                cover_url = get_cover_url(item, item.songs)
                for song in item.songs:
                    song_queue.append({
                        "title": song.name,
                        "artist": song.artist,
                        "album": item.name,
                        "search_term": f"{song.artist} - {song.name} Audio"
                    })
            elif "track" in query:
                song = Song.from_url(query)
                print(f"   Found Track: {song.name}")
                cover_url = get_cover_url(song)
                song_queue.append({
                    "title": song.name,
                    "artist": song.artist,
                    "album": song.album_name,
                    "search_term": f"{song.artist} - {song.name} Audio"
                })
        except Exception as e:
            print(f"Metadata Fetch Failed: {e}")
            return
    else:
        print(">> Detected Text Search.")
        song_queue.append({
            "title": query,
            "artist": "Unknown",
            "album": "Unknown Album",
            "search_term": query
        })

    print(f">> Queueing {len(song_queue)} tracks...")

    successful_tracks = []
    
    for i, item in enumerate(song_queue):
        print(f"[{i+1}/{len(song_queue)}] Downloading: {item['title']}")
        
        temp_file = f"temp_{i}.mp3"
        
        cmd = [
            'yt-dlp', 
            f'ytsearch1:{item["search_term"]}', 
            '-x', '--audio-format', 'mp3',
            '-o', f'{work_dir}/temp_{i}.%(ext)s', 
            '--no-playlist'
        ]
        
        if os.path.exists(COOKIE_FILE):
            cmd.extend(['--cookies', COOKIE_FILE])

        try:
            subprocess.check_call(cmd, cwd=work_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            
            full_temp_path = os.path.join(work_dir, temp_file)
            if os.path.exists(full_temp_path):
                
                try:
                    audio = EasyID3(full_temp_path)
                    audio['title'] = item['title']
                    audio['artist'] = item['artist']
                    audio['album'] = item['album']
                    audio.save()
                except Exception:
                    try:
                        audio = EasyID3()
                        audio['title'] = item['title']
                        audio['artist'] = item['artist']
                        audio['album'] = item['album']
                        audio.save(full_temp_path)
                    except: pass

                safe_title = sanitize_filename(item['title'])
                final_filename = f"{safe_title}.mp3"
                os.rename(full_temp_path, os.path.join(work_dir, final_filename))
                
                successful_tracks.append({
                    "file": final_filename,
                    "title": item['title']
                })
                
        except Exception as e:
            print(f"   Failed: {e}")

    if not successful_tracks:
        print("FAILURE: No tracks downloaded.")
        shutil.rmtree(work_dir)
        return

    print(">> Handling Cover Art...")
    final_cover_name = "a.png" 
    
    if cover_url:
        try:
            print(f"   Downloading High-Res Cover from Spotify...")
            req = urllib.request.Request(
                cover_url, 
                data=None, 
                headers={'User-Agent': 'Mozilla/5.0'}
            )
            with urllib.request.urlopen(req) as response, open(os.path.join(work_dir, final_cover_name), 'wb') as out_file:
                shutil.copyfileobj(response, out_file)
        except Exception as e:
            print(f"   Cover download failed: {e}")
    else:
        for f in os.listdir(work_dir):
            if f.lower().endswith(('.webp', '.jpg', '.jpeg', '.png')) and f != final_cover_name:
                print(f"   Using YouTube thumbnail: {f}")
                os.rename(os.path.join(work_dir, f), os.path.join(work_dir, final_cover_name))
                break

    album_name = song_queue[0]['album'] if song_queue else "Unknown Album"
    album_artist = song_queue[0]['artist'] if song_queue else "Unknown"
    final_folder_name = sanitize_filename(f"{album_artist}-{album_name}")
    
    if len(successful_tracks) == 1 and (album_name == "Unknown Album" or not is_url):
        final_folder_name = sanitize_filename(successful_tracks[0]['title'])

    metadata = {
        "albumTitle": album_name,
        "albumArtist": album_artist,
        "coverFile": final_cover_name,
        "tracks": successful_tracks
    }
    
    with open(os.path.join(work_dir, "metadata.json"), 'w') as f:
        json.dump(metadata, f, indent=4)

    destination = os.path.join(MUSIC_DIR, final_folder_name)
    if os.path.exists(destination): shutil.rmtree(destination)
    shutil.move(work_dir, destination)
    
    print(f"SUCCESS. Album ready at: {final_folder_name}")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        download_and_ingest(sys.argv[1])