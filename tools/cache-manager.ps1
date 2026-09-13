param([Parameter(Mandatory=$true)][string]$RequestFile)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

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

try {
    $request=[IO.File]::ReadAllText([IO.Path]::GetFullPath($RequestFile))|ConvertFrom-Json
    $root=Full-Directory ([string]$request.root)
    $files=@(List-Files $root)
    $result=[ordered]@{ok=$true;mode=[string]$request.mode;root=$root;message='';changed=0}
    switch([string]$request.mode) {
        'inspect' {
            $accounts=@{}; [long]$total=0; $imageCount=0; $detailCount=0; $latest=$null; $legacyCount=0
            foreach($file in $files) {
                $relative=Relative-Path $root $file.FullName
                if(-not (Cache-File $relative)){continue}; $total+=$file.Length
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
                if($remove -and $file.Name -notmatch '\.lock$|\.tmp-'){$selected+=$file.FullName}
            }
            foreach($file in $selected){$null=Child-Path $root (Relative-Path $root $file);Remove-Item -LiteralPath $file -Force;$result.changed++}
            $result.message='已删除 '+$result.changed+' 个缓存文件。当前已打开的内容可能暂留内存；需要时可重新刷新。'
        }
        'cleanup-legacy' {
            Add-Type -AssemblyName System.Drawing
            foreach($file in $files) {
                $relative=Relative-Path $root $file.FullName
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
            $target=[IO.Path]::GetFullPath([string]$request.destination)
            $null=Full-Directory ([IO.Path]::GetDirectoryName($target));$temp=$target+'.tmp-'+[guid]::NewGuid().ToString('N')
            $archive=$null
            try {
                $archive=[IO.Compression.ZipFile]::Open($temp,[IO.Compression.ZipArchiveMode]::Create)
                foreach($file in $files) {
                    $relative=Relative-Path $root $file.FullName
                    if(-not (Cache-File $relative) -or $relative -match '\.lock$|\.tmp-'){continue}
                    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$file.FullName,$relative,[IO.Compression.CompressionLevel]::Fastest)|Out-Null;$result.changed++
                }
                $archive.Dispose();$archive=$null
                if([IO.File]::Exists($target)){[IO.File]::Replace($temp,$target,[NullString]::Value)}else{[IO.File]::Move($temp,$target)}
            } finally {if($archive){$archive.Dispose()};if(Test-Path -LiteralPath $temp){Remove-Item -LiteralPath $temp -Force}}
            $result.message='备份完成，共 '+$result.changed+' 个文件。'
        }
        'restore' {
            $source=[IO.Path]::GetFullPath([string]$request.destination)
            $archive=[IO.Compression.ZipFile]::OpenRead($source)
            try {
                if($archive.Entries.Count -gt 100000){throw '备份条目数量异常。'}
                [long]$total=0;$seen=@{}
                foreach($entry in $archive.Entries){
                    if($entry.FullName.EndsWith('/')){continue};$total+=$entry.Length
                    if($entry.Length -gt 268435456 -or $total -gt 68719476736){throw '备份数据规模异常。'}
                    $relative=$entry.FullName.Replace('\','/');$target=Child-Path $root $relative
                    if(-not (Cache-File $relative) -or $relative -match '\.lock$|\.tmp-' -or $seen.ContainsKey($relative)){throw '备份包含非缓存或重复路径。'}
                    $seen[$relative]=$true
                }
                foreach($entry in $archive.Entries){
                    if($entry.FullName.EndsWith('/')){continue};$target=Child-Path $root $entry.FullName
                    if((Test-Path -LiteralPath $target) -and -not $request.overwrite){continue}
                    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))|Out-Null
                    $temp=$target+'.tmp-'+[guid]::NewGuid().ToString('N')
                    try {
                        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$temp,$false)
                        if([IO.File]::Exists($target)){[IO.File]::Replace($temp,$target,[NullString]::Value)}else{[IO.File]::Move($temp,$target)}
                    } finally {if(Test-Path -LiteralPath $temp){Remove-Item -LiteralPath $temp -Force}}
                    $result.changed++
                }
            } finally {$archive.Dispose()}
            $result.message='已恢复 '+$result.changed+' 个文件，下次启动后完整载入。'
        }
        'schedule-root' {
            $client=Full-Directory ([string]$request.clientRoot);$destination=Full-Directory ([string]$request.destination)
            if($destination -eq $root -or $destination.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase) -or $root.StartsWith($destination+'\',[StringComparison]::OrdinalIgnoreCase)){throw '新旧缓存目录不能相同或互相包含。'}
            if($request.copyExisting -and (Test-Path -LiteralPath $destination) -and @(Get-ChildItem -LiteralPath $destination -Force).Count){throw '迁移请选空目录；已有缓存目录请使用“直接切换”。'}
            if(-not $request.copyExisting -and (Test-Path -LiteralPath $destination) -and @(Get-ChildItem -LiteralPath $destination -Force).Count -and -not ((Test-Path -LiteralPath (Join-Path $destination 'accounts')) -or (Test-Path -LiteralPath (Join-Path $destination 'images')) -or (Test-Path -LiteralPath (Join-Path $destination 'catalog')))){throw '请选择空目录或已有的 KQPetData 缓存目录。'}
            $config=[ordered]@{schema=1;dataRoot=$root}
            if($request.copyExisting){$config.pendingRoot=$destination}else{$config.dataRoot=$destination;[IO.Directory]::CreateDirectory($destination)|Out-Null}
            Atomic-Json (Child-Path $client 'KQPetDataRoot.json') $config
            $result.message=if($request.copyExisting){'已安排迁移。退出并重新启动后复制缓存并切换目录，原目录保留。'}else{'已保存新缓存目录，下次启动生效。当前目录不会删除。'}
        }
        'migrate' {
            $client=Full-Directory ([string]$request.clientRoot);$destination=Full-Directory ([string]$request.destination)
            if($destination -eq $root -or $destination.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase) -or $root.StartsWith($destination+'\',[StringComparison]::OrdinalIgnoreCase)){throw '迁移路径不能互相包含。'}
            if((Test-Path -LiteralPath $destination) -and @(Get-ChildItem -LiteralPath $destination -Force).Count){throw '迁移目标不再是空目录。'}
            [IO.Directory]::CreateDirectory($destination)|Out-Null
            foreach($file in $files){$relative=Relative-Path $root $file.FullName;if($relative -match '\.lock$|\.tmp-|^\.maintenance/' -or -not ((Cache-File $relative) -or $relative -match '^(data-tools/|logs/)')){continue};Copy-Atomic $file.FullName (Child-Path $destination $relative);$result.changed++}
            Atomic-Json (Child-Path $client 'KQPetDataRoot.json') ([ordered]@{schema=1;dataRoot=$destination})
            $result.root=$destination;$result.message='迁移完成，原目录保留。'
        }
        default {throw '未知缓存管理操作。'}
    }
    $result|ConvertTo-Json -Depth 12 -Compress
} catch {
    [ordered]@{ok=$false;message=$_.Exception.Message}|ConvertTo-Json -Compress
    exit 1
}
