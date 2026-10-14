param([Parameter(Mandatory=$true)][string]$RequestFile)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Web.Extensions

function Full-Directory([string]$path) {
    if (-not [IO.Path]::IsPathRooted($path)) { throw '缓存目录必须是绝对路径。' }
    $full=[IO.Path]::GetFullPath($path).TrimEnd('\','/')
    if ($full.Length -le [IO.Path]::GetPathRoot($full).TrimEnd('\','/').Length) { throw '不能使用磁盘根目录作为缓存目录。' }
    $at=$full
    while ($at) {
        if (Test-Path -LiteralPath $at) {
            if ((Get-Item -LiteralPath $at -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw '缓存路径不能包含目录链接。' }
        }
        $parent=[IO.Path]::GetDirectoryName($at)
        if ($parent -eq $at) { break }; $at=$parent
    }
    return $full
}
function Child-Path([string]$root,[string]$relative) {
    if ([string]::IsNullOrWhiteSpace($relative) -or [IO.Path]::IsPathRooted($relative) -or $relative -match '[:*?"<>|\x00-\x1f]' -or ($relative -split '[\\/]' | Where-Object {$_ -in @('..','.','') -or $_ -match '[ .]$|^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)'})) { throw '缓存相对路径无效。' }
    $full=[IO.Path]::GetFullPath((Join-Path $root $relative))
    if (-not $full.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)) { throw '路径超出缓存目录。' }
    if ((Test-Path -LiteralPath $full) -and ((Get-Item -LiteralPath $full -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw '不能操作链接文件。' }
    $null=Full-Directory ([IO.Path]::GetDirectoryName($full))
    return $full
}
function List-Files([string]$root) {
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { return }
    $queue=[Collections.Generic.Queue[string]]::new(); $queue.Enqueue($root)
    while($queue.Count) {
        foreach($item in Get-ChildItem -LiteralPath $queue.Dequeue() -Force) {
            if($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw '缓存目录内含链接，已停止本次操作。' }
            if($item.PSIsContainer) { $queue.Enqueue($item.FullName) } else { $item }
        }
    }
}
function Relative-Path([string]$root,[string]$file) { $file.Substring($root.Length+1).Replace('\','/') }
function Cache-File([string]$relative) { $relative -match '^(accounts/|catalog/|images/|settings\.json$|last-account\.txt$)' }
function Public-File([string]$relative) {
    # Transaction workspaces and extraction scratch are reproducible work, not
    # the committed versions/evidence needed to restore the shared dataset.
    $relative -match '^public/' -and $relative -notmatch '^public/transactions/[^/]+/(workspace/|embedded\.brpack$)' -and $relative -notmatch '(?:^|/)(?:\.[^/]+|[^/]+\.lock)$|\.tmp-'
}
function Transfer-File([string]$relative) { (Cache-File $relative) -or (Public-File $relative) }
function Atomic-Json([string]$path,$value) {
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))|Out-Null
    $temp=$path+'.tmp-'+[guid]::NewGuid().ToString('N')
    try {
        [IO.File]::WriteAllText($temp,($value|ConvertTo-Json -Depth 12 -Compress),[Text.UTF8Encoding]::new($false))
        if([IO.File]::Exists($path)){[IO.File]::Replace($temp,$path,[NullString]::Value)}else{[IO.File]::Move($temp,$path)}
    } finally {if(Test-Path -LiteralPath $temp){Remove-Item -LiteralPath $temp -Force}}
}
function Copy-Atomic([string]$source,[string]$target) {
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))|Out-Null
    $temp=$target+'.tmp-'+[guid]::NewGuid().ToString('N')
    try {
        [IO.File]::Copy($source,$temp,$false)
        if((File-Hash $source) -ne (File-Hash $temp)){throw '复制过程中源文件变化，请稍后重试。'}
        if([IO.File]::Exists($target)){[IO.File]::Replace($temp,$target,[NullString]::Value)}else{[IO.File]::Move($temp,$target)}
    } finally {if(Test-Path -LiteralPath $temp){Remove-Item -LiteralPath $temp -Force}}
}
function File-Hash([string]$path) {
    $stream=[IO.File]::OpenRead($path);$sha=[Security.Cryptography.SHA256]::Create()
    try {return [Convert]::ToBase64String($sha.ComputeHash($stream))}finally{$sha.Dispose();$stream.Dispose()}
}
function Hex-Hash([string]$path) {
    $stream=[IO.File]::OpenRead($path);$sha=[Security.Cryptography.SHA256]::Create()
    try {return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose();$stream.Dispose()}
}
function Read-Object([string]$path,[long]$limit=134217728) {
    $file=Get-Item -LiteralPath $path -Force
    if($file.Length -le 0 -or $file.Length -gt $limit){throw '公共资料文件大小无效。'}
    $value=[IO.File]::ReadAllText($path,[Text.Encoding]::UTF8)|ConvertFrom-Json
    if($null -eq $value -or $value -isnot [pscustomobject]){throw '公共资料必须是 JSON 对象。'}
    return $value
}
function Test-PayloadObject([string]$path,[long]$limit=134217728) {
    $file=Get-Item -LiteralPath $path -Force
    if($file.Length -le 0 -or $file.Length -gt $limit){throw '公共资料文件大小无效。'}
    # PS5 ConvertFrom-Json projects every nested dictionary into PS properties.
    # Real skills contain psychicsSkills[""], a legal JSON key that cannot be a
    # PS property name. Ordinary payloads only need an object/parse check here;
    # keep their dictionaries intact instead of deleting or renaming keys.
    $reader=[System.Web.Script.Serialization.JavaScriptSerializer]::new()
    $reader.MaxJsonLength=134217728
    $reader.RecursionLimit=128
    $value=$reader.DeserializeObject([IO.File]::ReadAllText($path,[Text.Encoding]::UTF8))
    if($null -eq $value -or $value -isnot [Collections.IDictionary]){throw '公共资料必须是 JSON 对象。'}
}
function Protect-Image($state,[string]$relative) {
    $null=Child-Path $state.root $relative
    if($relative -notmatch '^images/(?:attributes|stargods|public-icons)/[A-Za-z0-9_-]+\.png$'){throw '公共图标引用路径无效。'}
    $state.protected[$relative]=$true
    $state.protected[[IO.Path]::ChangeExtension($relative,'.json')]=$true
}
function Check-Reference($state,$reference,[bool]$icons=$false,[int]$depth=0,[bool]$replay=$false) {
    if($depth -gt 16){throw '公共资料原始来源引用过深。'}
    $value=$null
    $digest=[string]$reference.sha256
    $limit=if($replay){134217728}else{33554432}
    if($digest -cnotmatch '^[0-9a-f]{64}$' -or $reference.format -cnotin @('json','brpack') -or
        (-not $replay -and $reference.format -cne 'json') -or ($depth -eq 0 -and -not $reference.origin.kind) -or
        $reference.bytes -le 0 -or $reference.bytes -gt $limit -or [long]$reference.bytes -ne $reference.bytes){throw '公共资料对象引用无效。'}
    $relative='public/objects/'+$digest+'/data';$path=Child-Path $state.root $relative
    if(-not $state.checked.ContainsKey($digest)) {
        if(-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -ne $reference.bytes -or (Hex-Hash $path) -cne $digest){throw '公共资料对象缺失或散列不符。'}
        $state.checked[$digest]=[long]$reference.bytes
    } elseif($state.checked[$digest] -ne $reference.bytes){throw '同一公共资料对象出现矛盾的长度。'}
    $state.required[$relative]=$digest
    if($reference.origin) {
        foreach($name in @('rawObject','embeddedObject')) {
            $nested=$reference.origin.PSObject.Properties[$name]
            if($nested -and $nested.Value){Check-Reference $state $nested.Value $false ($depth+1)}
        }
    }
    if($reference.format -eq 'json' -and -not $state.parsed.ContainsKey($digest)) {
        if($icons){$value=Read-Object $path}else{Test-PayloadObject $path}
        $state.parsed[$digest]=$true
    }
    if($icons -and -not $state.iconParsed.ContainsKey($digest)) {
        $index=if($null -ne $value){$value}else{Read-Object $path}
        $state.iconParsed[$digest]=$true
        if($index.available -eq $false){return}
        if($index.schemaVersion -ne 1 -or $null -eq $index.stargods){throw '公共图标索引结构无效。'}
        foreach($section in @('stargods','attributes')) {
            $rows=$index.PSObject.Properties[$section]
            if(-not $rows){continue}
            foreach($property in $rows.Value.PSObject.Properties) {
                $entry=$property.Value;$file=[string]$entry.file
                if(-not $file){$file='images/'+$section+'/'+$property.Name+'.png'}
                Protect-Image $state $file
                $expected=[string]$entry.png.sha256
                if($expected -and $expected -cnotmatch '^[0-9a-f]{64}$'){throw '公共图标内容身份无效。'}
                if($state.iconHashes.ContainsKey($file) -and $state.iconHashes[$file] -and $expected -and $state.iconHashes[$file] -cne $expected){throw '多个保留版本对同一图标路径记录了不同内容，未执行缓存操作。'}
                $state.iconHashes[$file]=$expected
            }
        }
    }
}
function Public-State([string]$root,$files) {
    $state=@{root=$root;pointerHash='';pointer=$null;protected=@{};required=@{};checked=@{};parsed=@{};iconParsed=@{};iconHashes=@{};manifests=@{}}
    $pointer=Child-Path $root 'public/current.json'
    if(Test-Path -LiteralPath $pointer -PathType Leaf) {
        $state.pointerHash=Hex-Hash $pointer;$state.pointer=Read-Object $pointer 16384
        if($state.pointer.schema -ne 1 -or $state.pointer.revision -cnotmatch '^[0-9a-f]{64}$' -or
            $state.pointer.generation -cnotmatch '^[0-9a-f]{32}$' -or ($state.pointer.previousRevision -and $state.pointer.previousRevision -cnotmatch '^[0-9a-f]{64}$')){throw '公共资料当前版本指针无效。'}
    }
    foreach($file in $files) {
        $relative=Relative-Path $root $file.FullName
        if($relative -notmatch '^public/releases/([0-9a-f]{64})/manifest\.json$'){continue}
        $revision=$Matches[1]
        if($state.manifests.Count -ge 10000){throw '公共版本数量超过检查上限。'}
        if((Hex-Hash (Child-Path $root $relative)) -cne $revision){throw '公共版本清单散列不符。'}
        $manifest=Read-Object $file.FullName 4194304
        $keys=@($manifest.components.PSObject.Properties.Name|Sort-Object)
        if($manifest.schema -ne 1 -or $manifest.dependencySchema -ne 1 -or ($keys -join ',') -cne 'battleReplay,icons,images,pets,routines,shop,skills'){throw '公共版本清单不完整。'}
        foreach($property in $manifest.components.PSObject.Properties) {
            if($property.Name -ne 'battleReplay' -and $property.Value.format -ne 'json'){throw '公共资料分块格式不匹配。'}
            Check-Reference $state $property.Value ($property.Name -eq 'icons') 0 ($property.Name -eq 'battleReplay')
        }
        foreach($property in $manifest.attachments.PSObject.Properties) {
            if($property.Name -notin @('activity-exchange-data.json','activity-evolution-data.json') -or $property.Value.format -ne 'json'){throw '公共资料附件未登记。'}
            Check-Reference $state $property.Value
        }
        $state.manifests[$revision]=$true;$state.required[$relative]=$revision
    }
    if($state.pointer) {
        foreach($revision in @($state.pointer.revision,$state.pointer.previousRevision)) {
            if($revision -and -not $state.manifests.ContainsKey($revision)){throw '当前或上一公共资料版本缺失。'}
        }
        # These are the read-only compatibility copy for program rollback.
        foreach($file in @('pet-detail-data.json','pet-skill-data.json','shop-exchange-data.json','pet-image-index.json','public-icon-index.json','routine-overview.json','battle-replay-catalog.json','battle-replay-catalog.brpack','activity-exchange-data.json','activity-evolution-data.json')){$state.protected['catalog/'+$file]=$true}
    }
    $state.parsed.Clear()
    return $state
}
function Public-Stable($state) {
    $pointer=Child-Path $state.root 'public/current.json'
    $now=if(Test-Path -LiteralPath $pointer -PathType Leaf){Hex-Hash $pointer}else{''}
    if($now -cne $state.pointerHash){throw '另一进程已切换公共资料，本次操作未提交；请重试。'}
}
function Bound-File($state,[string]$relative) {
    # There are no reliable cross-process image leases yet. Preserve every
    # content/source-addressed image, including retained pages from older runs.
    $state.protected.ContainsKey($relative) -or $relative -match '^public/|^images/public-icons/|^images/pets/sources/'
}
function Check-Transfer($state) {
    foreach($file in $state.iconHashes.Keys) {
        $path=Child-Path $state.root $file
        # A previously unavailable optional icon stays unavailable. Existing
        # pixels, however, must still match the index before being transferred.
        if((Test-Path -LiteralPath $path -PathType Leaf) -and $state.iconHashes[$file] -and (Hex-Hash $path) -cne $state.iconHashes[$file]){throw '现有图标与保留版本的内容身份不符，未提交备份或迁移。'}
    }
    Public-Stable $state
}
function Copy-Pointer([string]$source,[string]$target) {
    if(Test-Path -LiteralPath $target) {
        if((File-Hash $source) -ne (File-Hash $target)){throw '目标目录已有另一公共版本，未替换其当前指针。'}
        return
    }
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))|Out-Null
    $temporary=$target+'.tmp-'+[guid]::NewGuid().ToString('N')
    try {[IO.File]::Copy($source,$temporary,$false);[IO.File]::Move($temporary,$target)}
    finally {if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force}}
}
function Copy-Immutable([string]$source,[string]$target) {
    if(Test-Path -LiteralPath $target) {
        if((File-Hash $source) -ne (File-Hash $target)){throw '目标不可变文件已存在不同内容，未覆盖。'}
        return
    }
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))|Out-Null
    $temporary=$target+'.tmp-'+[guid]::NewGuid().ToString('N')
    try {
        [IO.File]::Copy($source,$temporary,$false)
        if((File-Hash $source) -ne (File-Hash $temporary)){throw '复制期间公共文件发生变化。'}
        try {[IO.File]::Move($temporary,$target)}catch {
            if(-not (Test-Path -LiteralPath $target -PathType Leaf) -or (File-Hash $target) -ne (File-Hash $temporary)){throw}
        }
    } finally {if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary -Force}}
}
function Immutable-File([string]$relative) { $relative -match '^public/(?:objects/|releases/|source-cache/evidence/)|^images/(?:public-icons/|pets/sources/)' }
function Restore-PublicVersion([string]$root,[string]$revision,[string]$helper) {
    if($revision -cnotmatch '^[0-9a-f]{64}$'){throw '备份公共版本身份无效。'}
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$helper;$start.Arguments='--restore-version "'+$root+'" '+$revision
    $start.WorkingDirectory=[IO.Path]::GetDirectoryName($helper)
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $start.StandardOutputEncoding=[Text.Encoding]::UTF8;$start.StandardErrorEncoding=[Text.Encoding]::UTF8
    $process=[Diagnostics.Process]::new();$process.StartInfo=$start
    try {
        if(-not $process.Start()){throw '公共资料恢复检查器无法启动。'}
        $output=$process.StandardOutput.ReadToEndAsync();$errors=$process.StandardError.ReadToEndAsync()
        if(-not $process.WaitForExit(180000)){$process.Kill();throw '公共资料恢复检查超时，未确认提交。'}
        $stdout=$output.GetAwaiter().GetResult();$stderr=$errors.GetAwaiter().GetResult()
        if($stdout.Length -gt 4194304 -or $stderr.Length -gt 4194304){throw '公共资料恢复检查结果超过大小上限。'}
        $report=$stdout|ConvertFrom-Json
        if($process.ExitCode -ne 0 -or -not $report.diskCommitted -or $report.revision -cne $revision -or $report.sessionApplied){
            $reason=[string]$report.error
            if(-not $reason){$reason='检查器没有确认完整公共版本已提交。'}
            throw ('公共资料未恢复：'+$reason)
        }
        return $report
    } finally {$process.Dispose()}
}

try {
    $request=[IO.File]::ReadAllText([IO.Path]::GetFullPath($RequestFile))|ConvertFrom-Json
    $root=Full-Directory ([string]$request.root)
    $files=@(List-Files $root)
    $public=Public-State $root $files
    $result=[ordered]@{ok=$true;mode=[string]$request.mode;root=$root;message='';changed=0}
    switch([string]$request.mode) {
        'inspect' {
            $accounts=@{}; [long]$total=0; $imageCount=0; $detailCount=0; $latest=$null; $legacyCount=0
            foreach($file in $files) {
                $relative=Relative-Path $root $file.FullName
                if(-not (Transfer-File $relative)){continue}; $total+=$file.Length
                if($relative -match '^accounts/([^/]+)/details/[^/]+\.json$') {
                    $account=$Matches[1]; if(-not $accounts.ContainsKey($account)){$accounts[$account]=[ordered]@{account=$account;details=0;bytes=0}}
                    $accounts[$account].details++; $accounts[$account].bytes+=$file.Length; $detailCount++
                }
                if($relative -match '^images/.+\.(png|jpg|jpeg|webp)$'){$imageCount++}
                if($relative -match '^images/pets/versions/'){$legacyCount++}
                if(-not $latest -or $file.LastWriteTimeUtc -gt $latest){$latest=$file.LastWriteTimeUtc}
            }
            $missing=0
            foreach($key in @($request.imageKeys|Select-Object -Unique)) {
                if($key -cmatch '^[0-9]+_[0-9]+$' -and -not (Test-Path -LiteralPath (Child-Path $root ('images/pets/'+$key+'.png')))){ $missing++ }
            }
            $result.summary=[ordered]@{bytes=$total;details=$detailCount;images=$imageCount;missingImages=$missing;legacyImageFiles=$legacyCount;latest=if($latest){$latest.ToString('o')}else{''};accounts=@($accounts.Values|ForEach-Object{[pscustomobject]$_}|Sort-Object account)}
            $result.message='缓存统计已更新。'
        }
        {$_ -in @('clear-account','clear-detail','clear-images','clear-image','clear-all')} {
            $account=[string]$request.account
            if($request.mode -in @('clear-account','clear-detail') -and $account -cnotmatch '^[A-Za-z0-9_-]+$'){throw '账号目录无效。'}
            $selected=@()
            $preserved=0
            foreach($file in $files) {
                $relative=Relative-Path $root $file.FullName
                $remove=$false
                if($request.mode -eq 'clear-images'){$remove=$relative.StartsWith('images/')}
                elseif($request.mode -eq 'clear-all'){$remove=$relative -match '^(images/|catalog/|accounts/[^/]+/(details/|derived/|snapshots/|inventory\.json$|shops\.json$|routines\.json$|cultivation-materials\.json$))'}
                elseif($request.mode -eq 'clear-account'){$remove=$relative -match ('^accounts/'+[regex]::Escape($account)+'/(details/|derived/|snapshots/|inventory\.json$|shops\.json$|routines\.json$|cultivation-materials\.json$)')}
                elseif($request.mode -eq 'clear-detail'){
                    $id=[string]$request.instanceId; if($id -cnotmatch '^[1-9][0-9]*$'){throw '请输入有效实例 ID。'}
                    $remove=$relative -in @(('accounts/'+$account+'/details/'+$id+'.json'),('accounts/'+$account+'/derived/'+$id+'.json'),('accounts/'+$account+'/derived/pets/'+$id+'.json'))
                } elseif($request.mode -eq 'clear-image') {
                    $target=Child-Path $root ([string]$request.relativeFile)
                    if(-not $target.StartsWith((Join-Path $root 'images')+'\',[StringComparison]::OrdinalIgnoreCase)){throw '只能删除图片缓存目录中的文件。'}
                    $remove=$file.FullName -eq $target -or $file.FullName -eq ($target+'.failure.json') -or $file.FullName -eq ([IO.Path]::ChangeExtension($target,'.json'))
                }
                if($remove -and (Bound-File $public $relative)){$preserved++;continue}
                if($remove -and $file.Name -notmatch '\.lock$|\.tmp-'){$selected+=$file.FullName}
            }
            Public-Stable $public
            foreach($file in $selected){$null=Child-Path $root (Relative-Path $root $file);Remove-Item -LiteralPath $file -Force;$result.changed++}
            $result.message='已删除 '+$result.changed+' 个缓存文件。当前已打开的内容可能暂留内存；需要时可重新刷新。'
            $result.preserved=$preserved
            if($preserved){$result.message+=' 已保留 '+$preserved+' 个公共资料或已绑定图片文件，供当前与旧回放继续使用。'}
        }
        'cleanup-legacy' {
            Public-Stable $public
            Add-Type -AssemblyName System.Drawing
            foreach($file in $files) {
                $relative=Relative-Path $root $file.FullName
                if(Bound-File $public $relative){continue}
                if($relative -notmatch '^images/pets/(?:versions/[^/]+/)?([0-9]+_[0-9]+)(?:_[^/]*)?\.png$'){continue}
                $stable=Child-Path $root ('images/pets/'+$Matches[1]+'.png')
                if($file.FullName -eq $stable -or -not (Test-Path -LiteralPath $stable)){continue}
                $image=$null
                try {$image=[Drawing.Image]::FromFile($stable)}catch{continue}finally{if($image){$image.Dispose()}}
                $null=Child-Path $root $relative;Remove-Item -LiteralPath $file.FullName -Force;$result.changed++
            }
            $result.message='已整理 '+$result.changed+' 个有稳定缓存替代的旧图片文件；其余文件保留。'
        }
        'backup' {
            Check-Transfer $public
            $target=[IO.Path]::GetFullPath([string]$request.destination)
            if($target.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase) -and (Bound-File $public (Relative-Path $root $target))){throw '备份目标不能覆盖公共资料或已绑定图片。'}
            $null=Full-Directory ([IO.Path]::GetDirectoryName($target));$temp=$target+'.tmp-'+[guid]::NewGuid().ToString('N')
            $archive=$null
            try {
                $archive=[IO.Compression.ZipFile]::Open($temp,[IO.Compression.ZipArchiveMode]::Create)
                foreach($file in $files) {
                    $relative=Relative-Path $root $file.FullName
                    if(-not (Transfer-File $relative) -or $relative -match '\.lock$|\.tmp-'){continue}
                    $before=if(Public-File $relative){File-Hash $file.FullName}else{''}
                    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$file.FullName,$relative,[IO.Compression.CompressionLevel]::Fastest)|Out-Null;$result.changed++
                    if($before -and $before -ne (File-Hash $file.FullName)){throw '公共资料在备份期间变化，未提交备份。'}
                }
                $archive.Dispose();$archive=$null
                Public-Stable $public
                if([IO.File]::Exists($target)){[IO.File]::Replace($temp,$target,[NullString]::Value)}else{[IO.File]::Move($temp,$target)}
            } finally {if($archive){$archive.Dispose()};if(Test-Path -LiteralPath $temp){Remove-Item -LiteralPath $temp -Force}}
            $result.message='备份完成，共 '+$result.changed+' 个文件。'
        }
        'restore' {
            $source=[IO.Path]::GetFullPath([string]$request.destination)
            $archive=[IO.Compression.ZipFile]::OpenRead($source)
            $stagingRelative='.maintenance/cache-restore-'+[guid]::NewGuid().ToString('N')
            $staging=Child-Path $root $stagingRelative
            try {
                if($archive.Entries.Count -gt 100000){throw '备份条目数量异常。'}
                [long]$total=0;$seen=@{}
                foreach($entry in $archive.Entries){
                    if($entry.FullName.EndsWith('/')){continue};$total+=$entry.Length
                    if($entry.Length -gt 268435456 -or $total -gt 68719476736){throw '备份数据规模异常。'}
                    $relative=$entry.FullName.Replace('\','/');$target=Child-Path $root $relative
                    if(-not (Transfer-File $relative) -or $relative -match '\.lock$|\.tmp-' -or $seen.ContainsKey($relative)){throw '备份包含非缓存或重复路径。'}
                    $seen[$relative]=$true
                }
                # Verify the complete archive before any existing user file is
                # replaced. The current pointer is never a per-file restore.
                foreach($entry in $archive.Entries){
                    if($entry.FullName.EndsWith('/')){continue};$target=Child-Path $staging $entry.FullName
                    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))|Out-Null
                    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$target,$false)
                }
                $stagedFiles=@(List-Files $staging);$staged=Public-State $staging $stagedFiles;Check-Transfer $staged;Check-Transfer $public
                $helper=Child-Path $root 'data-tools/scripts/KQPublicDatasetCheck.exe'
                if($staged.pointer -and -not (Test-Path -LiteralPath $helper -PathType Leaf)){throw '缺少同次构建的公共资料恢复检查器；未恢复任何账号或公共资料文件。'}
                $preserved=0
                foreach($file in $stagedFiles) {
                    $relative=Relative-Path $staging $file.FullName
                    if($public.iconHashes.ContainsKey($relative) -and $public.iconHashes[$relative] -and
                        (Hex-Hash $file.FullName) -cne $public.iconHashes[$relative]){throw '备份图标不符合当前公共资料的内容身份，未恢复文件。'}
                    $target=Child-Path $root $relative
                    if((Immutable-File $relative) -and
                        (Test-Path -LiteralPath $target) -and (File-Hash $file.FullName) -ne (File-Hash $target)){throw '不可变公共对象与目标目录冲突，未恢复文件。'}
                }
                foreach($phase in @(0,1)) {
                    if($phase -eq 1 -and $staged.pointer) {
                        $restored=Public-State $root @(List-Files $root);Check-Transfer $restored;Public-Stable $public
                        $publication=Restore-PublicVersion $root $staged.pointer.revision $helper
                        $result.publicRestoreRevision=$publication.revision
                        $result.publicRestoreAttempt=$publication.attempt
                        $result.diskCommitted=$true;$result.sessionApplied=$false
                    }
                    foreach($file in $stagedFiles){
                        $relative=Relative-Path $staging $file.FullName
                        if($relative -eq 'public/current.json'){continue}
                        $shared=(Public-File $relative) -or ($staged.pointer -and (Bound-File $staged $relative))
                        if(($phase -eq 0) -ne [bool]$shared){continue}
                        $target=Child-Path $root $relative
                        if((Test-Path -LiteralPath $target) -and (Bound-File $public $relative)) {
                            if((File-Hash $file.FullName) -ne (File-Hash $target)){$preserved++}
                            continue
                        }
                        if((Test-Path -LiteralPath $target) -and -not $request.overwrite){continue}
                        if(Immutable-File $relative){Copy-Immutable $file.FullName $target}else{Copy-Atomic $file.FullName $target}
                        $result.changed++
                    }
                }
                $restored=Public-State $root @(List-Files $root);Check-Transfer $restored
                if(-not $staged.pointer){Public-Stable $public}
                $result.preserved=$preserved
            } finally {
                $archive.Dispose()
                if(Test-Path -LiteralPath $staging) {
                    $checked=Full-Directory $staging
                    if($checked -ne (Child-Path $root $stagingRelative) -or $stagingRelative -cnotmatch '^\.maintenance/cache-restore-[0-9a-f]{32}$'){throw '恢复临时目录边界校验失败。'}
                    $null=@(List-Files $checked)
                    Remove-Item -LiteralPath $checked -Recurse -Force
                }
            }
            $result.message='已恢复 '+$result.changed+' 个文件，下次启动后完整载入。'
            if($staged.pointer){$result.message='备份文件已恢复，完整公共版本已通过原生检查并提交，等待当前会话应用。账号缓存下次启动载入。'}
            elseif($public.pointer){$result.message+=' 此旧备份不含公共资料版本，公共资料继续沿用当前版本。'}
            if($preserved){$result.message+=' 已保留 '+$preserved+' 个内容已绑定的历史资料文件。'}
        }
        'schedule-root' {
            $client=Full-Directory ([string]$request.clientRoot);$destination=Full-Directory ([string]$request.destination)
            if($destination -eq $root -or $destination.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase) -or $root.StartsWith($destination+'\',[StringComparison]::OrdinalIgnoreCase)){throw '新旧缓存目录不能相同或互相包含。'}
            if($request.copyExisting -and (Test-Path -LiteralPath $destination) -and @(Get-ChildItem -LiteralPath $destination -Force).Count){throw '迁移请选空目录；已有缓存目录请使用“直接切换”。'}
            if(-not $request.copyExisting -and (Test-Path -LiteralPath $destination) -and @(Get-ChildItem -LiteralPath $destination -Force).Count -and -not ((Test-Path -LiteralPath (Join-Path $destination 'accounts')) -or (Test-Path -LiteralPath (Join-Path $destination 'images')) -or (Test-Path -LiteralPath (Join-Path $destination 'catalog')) -or (Test-Path -LiteralPath (Join-Path $destination 'public/current.json')))){throw '请选择空目录或已有的 KQPetData 缓存目录。'}
            if(-not $request.copyExisting -and (Test-Path -LiteralPath $destination)){$targetState=Public-State $destination @(List-Files $destination);Check-Transfer $targetState}
            $config=[ordered]@{schema=1;dataRoot=$root}
            if($request.copyExisting){$config.pendingRoot=$destination}else{$config.dataRoot=$destination;[IO.Directory]::CreateDirectory($destination)|Out-Null}
            Atomic-Json (Child-Path $client 'KQPetDataRoot.json') $config
            $result.message=if($request.copyExisting){'已安排迁移。退出并重新启动后复制缓存并切换目录，原目录保留。'}else{'已保存新缓存目录，下次启动生效。当前目录不会删除。'}
        }
        'migrate' {
            Check-Transfer $public
            $client=Full-Directory ([string]$request.clientRoot);$destination=Full-Directory ([string]$request.destination)
            if($destination -eq $root -or $destination.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase) -or $root.StartsWith($destination+'\',[StringComparison]::OrdinalIgnoreCase)){throw '迁移路径不能互相包含。'}
            if((Test-Path -LiteralPath $destination) -and @(Get-ChildItem -LiteralPath $destination -Force).Count){throw '迁移目标不再是空目录。'}
            [IO.Directory]::CreateDirectory($destination)|Out-Null
            foreach($file in $files){$relative=Relative-Path $root $file.FullName;if($relative -eq 'public/current.json' -or $relative -match '\.lock$|\.tmp-|^\.maintenance/' -or -not ((Transfer-File $relative) -or $relative -match '^(data-tools/|logs/)')){continue};$target=Child-Path $destination $relative;if(Immutable-File $relative){Copy-Immutable $file.FullName $target}else{Copy-Atomic $file.FullName $target};$result.changed++}
            $copied=Public-State $destination @(List-Files $destination);Check-Transfer $copied;Public-Stable $public
            if($public.pointer){Copy-Pointer (Child-Path $root 'public/current.json') (Child-Path $destination 'public/current.json');$result.changed++}
            $copied=Public-State $destination @(List-Files $destination);Check-Transfer $copied;Public-Stable $public
            Atomic-Json (Child-Path $client 'KQPetDataRoot.json') ([ordered]@{schema=1;dataRoot=$destination})
            $result.root=$destination;$result.message='迁移完成，原目录保留。'
        }
        default {throw '未知缓存管理操作。'}
    }
    $result|ConvertTo-Json -Depth 12 -Compress
} catch {
    $failure=[ordered]@{ok=$false;message=$_.Exception.Message}
    if($result.publicRestoreRevision) {
        foreach($field in @('publicRestoreRevision','publicRestoreAttempt','diskCommitted','sessionApplied')){$failure[$field]=$result[$field]}
    }
    $failure|ConvertTo-Json -Compress
    exit 1
}
