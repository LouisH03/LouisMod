# Important
LouisMod executes local batch and PowerShell scripts, closes TrackMania, and launches multiple game instances. Review the scripts before using the mod.

## Automatic install

Press `Win+R`

Run:
```
powershell.exe -NoProfile -Command "$p=Join-Path $env:LOCALAPPDATA 'TMLoader\config.yaml';$u='https://louish03.github.io/LouisMod/';$line='  - '+[char]39+$u+[char]39;$lines=[System.Collections.ArrayList](Get-Content -LiteralPath $p);if(-not($lines -contains $line)){$i=$lines.IndexOf('servers:');if($i -ge 0){$j=$i+1;while($j -lt $lines.Count -and $lines[$j].TrimStart().StartsWith('-')){$j++};[void]$lines.Insert($j,$line)}else{[void]$lines.Add('servers:');[void]$lines.Add($line)};Set-Content -LiteralPath $p -Value $lines -Encoding UTF8}"
```
Press Enter

## Manual installation through TMLoader
Close TMLoader.

Open:
```text
%LocalAppData%\TMLoader\config.yaml
```

Add this under servers:
`- 'https://louish03.github.io/LouisMod/'`

Save the file and restart TMLoader.

Install LouisMod and enable it in your profile.

## Usage
*Recommended: Use TrackMania in windowed mode to avoid possible DirectX issues.*    
*[KimMod](https://home.koyaanis.com/modloader/) is also recommended for better Bruteforce performance*

Create a shortcut on the desktop for your Trackmania Modloader TAS profile. You can do this by clicking the arrow to the right side of the big Play button.

Create:
```
%USERPROFILE%\Documents\TrackMania\Tracks\Replays\Bruteforce
```

Save your replay to the folder

Change your bruteforce settings
Set the instance count and click `MULTIBRUTEFORCE`.
Trackmania will close to save your BF settings, then reopen the selected number of instances.
The replay in the Bruteforce folder will begin automatically bruteforcing.

A window will appear showing you all of the results that have been found. The controls are visible, but you can sort by different stats or copy a file directly by pressing the corresponding number next to the ID.

The result will be copied to `results.txt` which will be automatically loaded in an additional instance on Trackmania. 
When you are happy with the results, tab into the results checker and press F to close every currently bruteforcing instance, leaving the extra for you to change settings and restart the process if need be.

## Requirements

- TrackMania Nations Forever or United Forever
- [TMLoader](https://tomashu.dev/software/tmloader/)
- CoreMod
- TMInterface
- Windows PowerShell (sorry linux users)

## License

LouisMod is released under the MIT License.

Dear ImGui is used under its own MIT License.
