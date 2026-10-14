package mmo.activityext.newact20260522.farmv2.model
{
   import com.adobe.utils.StringUtil;
   import com.hurlant.crypto.Crypto;
   import com.hurlant.util.Hex;
   import flash.utils.ByteArray;
   import mmo.common.helper.ActUtil;
   import mmo.common.user.UserInfo;
   import mmo.common.utils.dt.mvc.Model;
   import mmo.config.ConfigReader;
   import mmo.interfaces.ServiceContainer;
   import mmo.interfaces.material.IMaterialService;
   import mmo.interfaces.material.MaterialTypes;
   
   public class FV2Model extends Model
   {
      
      private var _isFirst:Boolean;
      
      private var _refreshTimes:int;
      
      private var _refreshTimesMax:int;
      
      private var _pickTimes:int;
      
      private var _pickTimesMax:int;
      
      private var _shard:int;
      
      private var _farms:Vector.<FV2FarmData>;
      
      private var _invites:Array;
      
      private var _inviteCount:int;
      
      private var _inviteLink:String;
      
      private var _inviteAccepted:Boolean;
      
      private var _convertableItems:Array;
      
      private var _weeklyBuyPickCount:int;
      
      private var _isGetFlower:Boolean;
      
      private var _weekPickTimes:int;
      
      private var _curFarmIndex:int = -1;
      
      private var _curFields:Vector.<FV2FieldData>;
      
      private var _pickedCrops:Array;
      
      private var _settleResult:Object;
      
      private var _chestIds:Array;
      
      private var _chestStarGodStr:Array;
      
      public function FV2Model()
      {
         super(FV2ModelEvent);
         this._farms = new Vector.<FV2FarmData>();
         this._invites = [];
         this._convertableItems = [];
         this._pickedCrops = [];
      }
      
      public function dispose() : void
      {
      }
      
      public function parseData(param1:Object) : void
      {
         var _loc5_:FV2FarmData = null;
         this._weekPickTimes = int(param1["wp"]) || 0;
         this._isGetFlower = param1["gfn"];
         this._inviteAccepted = param1["is"];
         this._isFirst = param1["isf"];
         this._invites = param1["invites"] || [];
         this._inviteCount = param1["inviteCount"] != null ? int(param1["inviteCount"]) : int(this._invites.length);
         var _loc2_:Object = FV2Config.getInviteBonus(this._inviteCount);
         this._refreshTimes = param1["rt"];
         this._refreshTimesMax = _loc2_["refreshMax"];
         this._pickTimes = param1["pt"];
         this._pickTimesMax = _loc2_["pickMax"];
         if(param1["shard"] != null)
         {
            this._shard = param1["shard"];
         }
         else
         {
            this._shard = this._queryShardCount();
         }
         if(param1["inviteLink"] != null)
         {
            this._inviteLink = param1["inviteLink"];
         }
         else
         {
            this._inviteLink = this._generateInviteLink();
         }
         if(param1["convertable"] != null)
         {
            this._convertableItems = param1["convertable"];
         }
         else
         {
            this._convertableItems = this._queryConvertableItems();
         }
         this._weeklyBuyPickCount = param1["wbp"] != null ? int(param1["wbp"]) : 0;
         this._farms = new Vector.<FV2FarmData>();
         var _loc3_:Array = param1["farms"] || [];
         var _loc4_:int = 0;
         while(_loc4_ < _loc3_.length)
         {
            if(_loc4_ >= FV2Config.FARM_COUNT)
            {
               break;
            }
            _loc5_ = new FV2FarmData();
            _loc5_.parse(_loc4_,_loc3_[_loc4_]);
            this._farms.push(_loc5_);
            _loc4_++;
         }
         dispatchEventX(FV2ModelEvent.UPDATE);
      }
      
      private function _generateInviteLink() : String
      {
         var _loc1_:String = ConfigReader.isTesting ? "http://www.pjx.com" : "https://aoqi.100bt.com";
         var _loc2_:String = FV2Config.INVITE_QS_KEY;
         var _loc3_:uint = uint(UserInfo.userName);
         return _loc1_ + "/?" + _loc2_ + "=" + this.encryptByDES(String(_loc3_),FV2Config.KEY);
      }
      
      public function decryptByDES(param1:String, param2:String) : String
      {
         if(StringUtil.isNullOrEmpty(param1))
         {
            return param1;
         }
         if(StringUtil.isNullOrEmpty(param2))
         {
            throw new Error("key is null or empty");
         }
         var _loc3_:ByteArray = new ByteArray();
         _loc3_.writeUnsignedInt(uint(param1));
         Crypto.getCipher("rc4",Hex.toArray(param2)).decrypt(_loc3_);
         _loc3_.position = 0;
         return _loc3_.readUnsignedInt().toString();
      }
      
      private function encryptByDES(param1:String, param2:String) : String
      {
         if(StringUtil.isNullOrEmpty(param1))
         {
            return param1;
         }
         if(StringUtil.isNullOrEmpty(param2))
         {
            throw new Error("key is null or empty");
         }
         var _loc3_:uint = uint(param1);
         var _loc4_:ByteArray = new ByteArray();
         _loc4_.writeUnsignedInt(_loc3_);
         Crypto.getCipher("rc4",Hex.toArray(param2)).encrypt(_loc4_);
         _loc4_.position = 0;
         return _loc4_.readUnsignedInt().toString();
      }
      
      private function _queryShardCount() : int
      {
         var _loc1_:Array = FV2Config.SHARD_ID.split(":");
         var _loc2_:int = int(_loc1_[0]);
         var _loc3_:int = int(_loc1_[1]);
         return ActUtil.getMyMaterilaById(_loc2_,_loc3_);
      }
      
      private function _queryConvertableItems() : Array
      {
         var _loc4_:Array = null;
         var _loc5_:Object = null;
         var _loc6_:String = null;
         var _loc7_:String = null;
         var _loc8_:int = 0;
         var _loc9_:String = null;
         var _loc10_:String = null;
         var _loc11_:Array = null;
         var _loc12_:Object = null;
         var _loc13_:String = null;
         var _loc14_:String = null;
         var _loc15_:int = 0;
         var _loc16_:String = null;
         var _loc17_:String = null;
         var _loc1_:Array = [];
         var _loc2_:IMaterialService = ServiceContainer.getMaterialService(MaterialTypes.PLANT_SEED);
         if(Boolean(_loc2_))
         {
            _loc4_ = _loc2_.getMyMaterials();
            if(Boolean(_loc4_) && _loc4_.length > 0)
            {
               for each(_loc5_ in _loc4_)
               {
                  _loc6_ = _loc5_["materialType"];
                  _loc7_ = _loc5_["materialId"];
                  _loc8_ = int(_loc5_["quantity"]);
                  if(!(!_loc7_ || _loc8_ <= 0))
                  {
                     _loc9_ = _loc6_ + ":" + _loc7_;
                     if(FV2Config.SEED_CONVERT_RULES.hasOwnProperty(_loc9_))
                     {
                        _loc10_ = FV2Config.SEED_CONVERT_RULES[_loc9_] as String;
                        _loc1_.push({
                           "itemId":_loc9_,
                           "count":_loc8_,
                           "convertType":"seed",
                           "rewardConfig":_loc10_
                        });
                     }
                  }
               }
            }
         }
         var _loc3_:IMaterialService = ServiceContainer.getMaterialService(MaterialTypes.PLANT_ITEM);
         if(Boolean(_loc3_))
         {
            _loc11_ = _loc3_.getMyMaterials();
            if(Boolean(_loc11_) && _loc11_.length > 0)
            {
               for each(_loc12_ in _loc11_)
               {
                  _loc13_ = _loc12_["materialType"];
                  _loc14_ = _loc12_["materialId"];
                  _loc15_ = int(_loc12_["quantity"]);
                  if(!(!_loc14_ || _loc15_ <= 0))
                  {
                     _loc16_ = _loc13_ + ":" + _loc14_;
                     if(FV2Config.FIELD_CONVERT_RULES.hasOwnProperty(_loc16_))
                     {
                        _loc17_ = FV2Config.FIELD_CONVERT_RULES[_loc16_] as String;
                        _loc1_.push({
                           "itemId":_loc16_,
                           "count":_loc15_,
                           "convertType":"field",
                           "rewardConfig":_loc17_
                        });
                     }
                  }
               }
            }
         }
         return _loc1_;
      }
      
      public function afterRefresh(param1:Object) : void
      {
         var _loc4_:FV2FarmData = null;
         var _loc2_:Array = param1["farms"] || [];
         this._farms = new Vector.<FV2FarmData>();
         var _loc3_:int = 0;
         while(_loc3_ < _loc2_.length)
         {
            if(_loc3_ >= FV2Config.FARM_COUNT)
            {
               break;
            }
            _loc4_ = new FV2FarmData();
            _loc4_.parse(_loc3_,_loc2_[_loc3_]);
            this._farms.push(_loc4_);
            _loc3_++;
         }
         this._refreshTimes = Math.max(0,this._refreshTimes - 1);
         this._shard = param1["shard"] != null ? int(param1["shard"]) : this._shard;
         dispatchEventX(FV2ModelEvent.FARM_REFRESHED);
      }
      
      public function afterEnterFarm(param1:int, param2:Object) : void
      {
         var _loc4_:Object = null;
         var _loc5_:FV2FieldData = null;
         this._curFarmIndex = param1;
         this._curFields = new Vector.<FV2FieldData>();
         this._pickedCrops = [];
         this._chestIds = param2["box"] != null ? param2["box"] : [];
         var _loc3_:Array = param2["fields"] || [];
         for each(_loc4_ in _loc3_)
         {
            _loc4_["level"] = (this._farms[param1] as FV2FarmData).level;
            _loc5_ = new FV2FieldData();
            _loc5_.parse(_loc4_);
            this._curFields.push(_loc5_);
         }
         dispatchEventX(FV2ModelEvent.UPDATE);
      }
      
      public function afterPickField(param1:int, param2:Number) : void
      {
         var _loc3_:FV2CropData = null;
         if(Boolean(this._curFields) && param1 < this._curFields.length)
         {
            this._curFields[param1].applyPickResult(param2);
            _loc3_ = new FV2CropData();
            _loc3_.parse((this._curFields[param1] as FV2FieldData).cropId,param2);
            this._pickedCrops.push(_loc3_);
         }
         dispatchEventX(FV2ModelEvent.FIELD_PICKED,{"fieldIndex":param1});
      }
      
      public function afterOpenChest(param1:Object) : void
      {
         dispatchEventX(FV2ModelEvent.CHEST_OPENED);
      }
      
      public function afterSettle(param1:Object) : void
      {
         this._settleResult = param1;
         this._pickTimes = int(param1["pt"]) || this._pickTimes;
         var _loc2_:int = param1["farmIndex"] != null ? int(param1["farmIndex"]) : this._curFarmIndex;
         if(_loc2_ >= 0 && _loc2_ < this._farms.length)
         {
            this._farms[_loc2_].picked = true;
         }
         this._curFarmIndex = -1;
         this._curFields = null;
         this._pickedCrops = [];
         this._chestIds = [];
         this._chestStarGodStr = [];
         dispatchEventX(FV2ModelEvent.SETTLED);
      }
      
      public function afterBuyPickTimes(param1:Object) : void
      {
         this._pickTimes = param1["pt"] != null ? int(param1["pt"]) : int(this._pickTimes + 1);
         this._pickTimesMax = param1["ptMax"] != null ? int(param1["ptMax"]) : this._pickTimesMax;
         this._weeklyBuyPickCount = param1["wbp"] != null ? int(param1["wbp"]) : int(this._weeklyBuyPickCount + 1);
         dispatchEventX(FV2ModelEvent.UPDATE);
      }
      
      public function afterBuyRefreshTimes(param1:Object) : void
      {
         this.afterRefresh(param1);
      }
      
      public function afterShardRefresh(param1:Object) : void
      {
         this._shard = Math.max(0,this._shard - FV2Config.SHARD_PER_REFRESH);
         this.afterRefresh(param1);
      }
      
      public function afterConvertItems(param1:Object) : void
      {
         this._shard = this._queryShardCount();
         this._convertableItems = this._queryConvertableItems();
         dispatchEventX(FV2ModelEvent.UPDATE);
      }
      
      public function afterGetFlower(param1:Object) : void
      {
         this._isGetFlower = true;
         dispatchEventX(FV2ModelEvent.UPDATE);
      }
      
      public function get chestStarGodStr() : Array
      {
         return this._chestStarGodStr;
      }
      
      public function set chestStarGodStr(param1:Array) : void
      {
         this._chestStarGodStr = param1;
      }
      
      public function get weekPickTimes() : int
      {
         return this._weekPickTimes;
      }
      
      public function get isGetFlower() : Boolean
      {
         return this._isGetFlower;
      }
      
      public function get inviteAccepted() : Boolean
      {
         return this._inviteAccepted;
      }
      
      public function set isFirst(param1:Boolean) : void
      {
         this._isFirst = param1;
      }
      
      public function get isFirst() : Boolean
      {
         return this._isFirst;
      }
      
      public function get refreshTimes() : int
      {
         return this._refreshTimes;
      }
      
      public function get refreshTimesMax() : int
      {
         return this._refreshTimesMax;
      }
      
      public function get weeklyBuyPickCount() : int
      {
         return this._weeklyBuyPickCount;
      }
      
      public function get isPickBuyLimited() : Boolean
      {
         return this._weeklyBuyPickCount >= FV2Config.BUY_PICK_PRICES.length;
      }
      
      public function get currentPickBuyPrice() : int
      {
         return FV2Config.getPickBuyPrice(this._weeklyBuyPickCount);
      }
      
      public function get pickTimes() : int
      {
         return this._pickTimes;
      }
      
      public function get pickTimesMax() : int
      {
         return this._pickTimesMax;
      }
      
      public function get shard() : int
      {
         return this._shard;
      }
      
      public function get farms() : Vector.<FV2FarmData>
      {
         return this._farms;
      }
      
      public function get invites() : Array
      {
         return this._invites;
      }
      
      public function get inviteCount() : int
      {
         return this._inviteCount;
      }
      
      public function get inviteLink() : String
      {
         return this._inviteLink;
      }
      
      public function get convertableItems() : Array
      {
         return this._convertableItems;
      }
      
      public function get curFarmIndex() : int
      {
         return this._curFarmIndex;
      }
      
      public function get curFields() : Vector.<FV2FieldData>
      {
         return this._curFields;
      }
      
      public function get pickedCrops() : Array
      {
         return this._pickedCrops;
      }
      
      public function get settleResult() : Object
      {
         return this._settleResult;
      }
      
      public function get hasConvertable() : Boolean
      {
         return Boolean(this._convertableItems) && this._convertableItems.length > 0;
      }
      
      public function get hasChest() : Boolean
      {
         return this._chestIds.length > 0;
      }
      
      public function get chestIds() : Array
      {
         return this._chestIds;
      }
      
      public function get allFieldsOpened() : Boolean
      {
         var _loc1_:FV2FieldData = null;
         if(!this._curFields)
         {
            return false;
         }
         for each(_loc1_ in this._curFields)
         {
            if(!_loc1_.opened)
            {
               return false;
            }
         }
         return true;
      }
      
      public function getFarmByIndex(param1:int) : FV2FarmData
      {
         if(param1 < 0 || param1 >= this._farms.length)
         {
            return null;
         }
         return this._farms[param1];
      }
      
      public function get inviteBonusConfig() : Object
      {
         return FV2Config.getInviteBonus(this._inviteCount);
      }
      
      public function getChestRewards() : Array
      {
         var allRewards:Array = null;
         var result:Array = null;
         if(this._chestIds.length <= 0)
         {
            return [];
         }
         allRewards = FV2Config.CHEST_REWARDS;
         result = [];
         this._chestIds.forEach(function(param1:int, ... rest):*
         {
            var _loc3_:Object = null;
            for each(_loc3_ in allRewards)
            {
               if(_loc3_["id"] == param1)
               {
                  result.push({"itemId":_loc3_["itemId"]});
               }
            }
         });
         return result;
      }
      
      public function buildSettleResult(param1:int) : Object
      {
         var _loc5_:FV2CropData = null;
         var _loc2_:int = 0;
         var _loc3_:int = 0;
         var _loc4_:int = 0;
         for each(_loc5_ in this._pickedCrops)
         {
            _loc2_ += _loc5_.value;
            if(_loc5_.category == 1)
            {
               _loc3_ += _loc5_.value;
            }
            if(_loc5_.category == 2)
            {
               _loc4_ += _loc5_.value;
            }
         }
         return {
            "farmIndex":param1,
            "crops":this.getSortedCrops(),
            "totalValue":_loc2_,
            "value1":_loc3_,
            "value2":_loc4_,
            "hasChest":this._chestIds.length > 0,
            "chestRewards":this.getChestRewards(),
            "pt":this._pickTimes
         };
      }
      
      public function getSortedCrops() : Array
      {
         var _loc1_:Array = this._pickedCrops.concat();
         _loc1_.sortOn(["rarity","cropId"],[Array.DESCENDING | Array.NUMERIC,Array.DESCENDING]);
         return _loc1_;
      }
   }
}

